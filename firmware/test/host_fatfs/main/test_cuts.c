// The FatFs power-cut test for cardfs.c: IDF's FatFs and VFS over a RAM disk that cuts the power at every sector
// write of an operation, with and without a torn sector, then remounts, recovers and checks the volume. For small
// files it also cuts the recovery at every one of its writes and recovers again.
//
// After each recovery: the operation's file is old or new (an image absent, valid, or detected invalid and
// deleted), no intent record holds anything, every other file and directory is unchanged, and fatwalk finds no
// cross-linked or broken chain. Then the operation runs again to completion and the same checks hold for the new
// state.
//
// Growing a directory isn't cut-safe in FatFs: dir_next links the new cluster (create_chain), and dir_clear flushes
// that link before it zeroes the cluster, so a cut between them leaves stale bytes in the directory. fsck.fat reads
// them as bad entries. With a free entry before them, FatFs stops at that end marker and doesn't see them, but it can
// place a later entry past them, where no lookup finds it; cardfs then fails the write rather than lose it. The
// "nearly full" cards exercise that: the file stays old, and the operation may be refused. A directory with no free
// entry at all shows the stale bytes to FatFs as well, and no test here covers it: cardfs can't avoid it, it needs
// that directory's first new entry in a cluster that is exactly full, and records are emptied, never deleted, so
// they never need a new entry after their first write.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cardfs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "fatwalk.h"
#include "ff.h"
#include "psa/crypto.h"
#include "ramdisk.h"
#include "unity.h"

#define ROOT "/card"
#define DISK_SECTORS 70000 // FAT32 needs over 65,525 clusters; 512-byte clusters keep the disk at 35 MB
#define TREE_MAX 96
#define IMAGE_LEN (1024 * 1024)
#define DIR_SLOTS (RAMDISK_SECTOR / 32) // directory entries in one cluster

static FATFS *s_fs;
static char s_drive[3];

// As in host_proto: on the linux target the static link leaves mbedtls's heap hooks unresolved once PSA is used
// (esp_mem.c sits in an archive that the linker has already passed). Supply them with libc here.
void *esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

void esp_mbedtls_mem_free(void *ptr)
{
    free(ptr);
}

typedef struct {
    char path[300];
    bool is_dir;
    size_t size;
    uint8_t sha[32];
} entry_t;

typedef struct {
    entry_t e[TREE_MAX];
    int n;
} tree_t;

typedef enum { CARD_FRESH, CARD_OLD_FILES, CARD_EMPTY_WATCHDECK } card_t;

typedef struct {
    const char *name;
    card_t card;
    const char *fill; // NULL, "" (the root) or "watchdeck": filled until its last cluster has fill_left free entries
    int fill_left;
    const char *target; // relative to ROOT
    const uint8_t *data;
    size_t len;
    bool image; // write-once with cardfs_write_new, checked with cardfs_verify
    bool grows; // the operation makes FatFs grow a directory, so some cuts leave stale bytes in it
    uint8_t sha[32];
} op_t;

// Totals over every cut point of one operation.
typedef struct {
    uint32_t writes, cuts_whole, cuts_torn, recovery_cuts;
    uint32_t old, new_, img_absent, img_valid, img_invalid;
    uint32_t lost, fat_differ, fsinfo_stale, hidden, refused;
} stats_t;

static stats_t s_stats;
static char s_where[160];

static void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...)
{
    static char msg[512];
    int n = snprintf(msg, sizeof(msg), "%s: ", s_where);
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg + n, sizeof(msg) - n, fmt, args);
    va_end(args);
    TEST_FAIL_MESSAGE(msg);
}

static void sha256(const void *data, size_t len, uint8_t out[32])
{
    size_t out_len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &out_len));
}

static void mount(void)
{
    const FRESULT res = f_mount(s_fs, s_drive, 1);
    if (res != FR_OK) {
        fail("mount: FRESULT %d", res);
    }
}

static void unmount(void)
{
    f_mount(NULL, s_drive, 0);
}

static void plant(const char *rel, size_t len, uint8_t seed)
{
    char path[128];
    snprintf(path, sizeof(path), ROOT "/%s", rel);
    uint8_t *buf = malloc(len + 1);
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i * 31 + (i >> 8));
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL(len, write(fd, buf, len));
    TEST_ASSERT_EQUAL(0, close(fd));
    free(buf);
}

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// Free entries in the last cluster of the directory at cluster `start`, read from the disk.
static int free_slots(uint32_t start)
{
    const uint8_t *bs = ramdisk_sector(0);
    const uint32_t fatbase = bs[14] | bs[15] << 8;
    const uint32_t database = fatbase + bs[16] * le32(bs + 36);
    uint32_t c = start;
    for (uint32_t next; (next = le32(ramdisk_sector(fatbase + c / 128) + c % 128 * 4) & 0x0FFFFFFF) < 0x0FFFFFF8;) {
        c = next;
    }
    const uint8_t *d = ramdisk_sector(database + c - 2);
    int n = 0;
    for (int e = 0; e < DIR_SLOTS; e++) {
        n += d[e * 32] == 0;
    }
    return n;
}

// The start cluster of ROOT/<dir>, or of the root for "".
static uint32_t dir_cluster(const char *dir)
{
    if (dir[0] == '\0') {
        return le32(ramdisk_sector(0) + 44);
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", s_drive, dir);
    FF_DIR d;
    TEST_ASSERT_EQUAL(FR_OK, f_opendir(&d, path));
    const uint32_t cl = d.obj.sclust;
    f_closedir(&d);
    return cl;
}

// Plants empty files (one entry each, no cluster) in dir until its last cluster has `left` free entries.
static void fill_dir(const char *dir, int left)
{
    const uint32_t cl = dir_cluster(dir);
    for (int i = 0; free_slots(cl) != left; i++) {
        TEST_ASSERT_TRUE(i < 4 * DIR_SLOTS);
        char rel[64];
        snprintf(rel, sizeof(rel), "%s%sF%03d", dir, dir[0] ? "/" : "", i);
        plant(rel, 0, 0);
    }
}

static void hash_file(const char *path, size_t *size, uint8_t sha[32])
{
    static uint8_t buf[CARDFS_PIECE];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fail("open %s: errno %d", path, errno);
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_setup(&op, PSA_ALG_SHA_256));
    *size = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        *size += (size_t)n;
        TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_update(&op, buf, (size_t)n));
    }
    close(fd);
    if (n < 0) {
        fail("read %s: errno %d", path, errno);
    }
    size_t len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_finish(&op, sha, 32, &len));
}

// Every file and directory under ROOT, through FatFs, each file with its SHA-256.
static void scan_dir(const char *rel, tree_t *t)
{
    char path[320];
    snprintf(path, sizeof(path), ROOT "%s%s", rel[0] ? "/" : "", rel);
    DIR *dir = opendir(path);
    if (!dir) {
        fail("opendir %s: errno %d", path, errno);
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (t->n == TREE_MAX) {
            fail("more than %d entries", TREE_MAX);
        }
        entry_t *e = &t->e[t->n++];
        snprintf(e->path, sizeof(e->path), "%s%s%s", rel, rel[0] ? "/" : "", ent->d_name);
        snprintf(path, sizeof(path), ROOT "/%s", e->path);
        struct stat st;
        if (stat(path, &st) != 0) {
            fail("stat %s: errno %d", path, errno);
        }
        e->is_dir = S_ISDIR(st.st_mode);
        if (e->is_dir) {
            scan_dir(e->path, t);
        } else {
            hash_file(path, &e->size, e->sha);
        }
    }
    closedir(dir);
}

static void scan(tree_t *t)
{
    t->n = 0;
    scan_dir("", t);
}

static const entry_t *find(const tree_t *t, const char *path)
{
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->e[i].path, path) == 0) {
            return &t->e[i];
        }
    }
    return NULL;
}

static bool same_entry(const entry_t *a, const entry_t *b)
{
    return a->is_dir == b->is_dir && a->size == b->size && memcmp(a->sha, b->sha, 32) == 0;
}

static bool is_record(const char *path)
{
    return strcmp(path, "watchdeck/wifi.new") == 0 || strcmp(path, "watchdeck/sleep.new") == 0;
}

// Every path but the target is as it was, except that watchdeck/ and a record may have appeared, and a record must
// be empty unless a recovery failed (and left it for the next mount). Nothing else appeared.
static void compare(const tree_t *before, const tree_t *after, const op_t *op, bool recovery_failed)
{
    for (int i = 0; i < before->n; i++) {
        const entry_t *b = &before->e[i];
        if (strcmp(b->path, op->target) == 0) {
            continue;
        }
        const entry_t *a = find(after, b->path);
        if (!a) {
            fail("%s vanished", b->path);
        }
        if (!same_entry(a, b)) {
            fail("%s changed (%u bytes)", b->path, (unsigned)a->size);
        }
    }
    for (int i = 0; i < after->n; i++) {
        const entry_t *a = &after->e[i];
        if (is_record(a->path) && a->size != 0 && !recovery_failed) {
            fail("%s holds %u bytes after recovery", a->path, (unsigned)a->size);
        }
        if (find(before, a->path) || strcmp(a->path, op->target) == 0 || is_record(a->path)) {
            continue;
        }
        if (!(strcmp(a->path, "watchdeck") == 0 && a->is_dir)) {
            fail("%s appeared", a->path);
        }
    }
}

static esp_err_t run_op(const op_t *op)
{
    return op->image ? cardfs_write_new(op->target, op->data, op->len)
                     : cardfs_write_small(op->target, op->data, op->len);
}

static void check_volume(fatwalk_t *w)
{
    if (!fatwalk(w)) {
        fail("fatwalk: %s", w->problem);
    }
    DWORD free_clusters;
    FATFS *fs;
    const FRESULT res = f_getfree(s_drive, &free_clusters, &fs);
    if (res != FR_OK) {
        fail("f_getfree: FRESULT %d", res);
    }
}

// True when the small file is new, false when it's old, and a failure when it's neither.
static bool small_is_new(const op_t *op, const tree_t *before, const tree_t *after)
{
    const entry_t *now = find(after, op->target);
    const entry_t *old = find(before, op->target);
    entry_t want = {.size = op->len};
    memcpy(want.sha, op->sha, 32);
    if (now && same_entry(now, &want)) {
        return true;
    }
    if (!((!now && !old) || (now && old && same_entry(now, old)))) {
        fail("%s is neither old nor new (%s, %u bytes)", op->target, now ? "present" : "absent",
             now ? (unsigned)now->size : 0);
    }
    return false;
}

// The target after a recovery: old or new for a small file; absent, valid, or invalid and then deleted for an image.
static void check_target(const op_t *op, const tree_t *before, const tree_t *after)
{
    if (op->image) {
        const esp_err_t v = cardfs_verify(op->target, op->len, op->sha, 16);
        if (v == ESP_ERR_NOT_FOUND) {
            s_stats.img_absent++;
        } else if (v == ESP_OK) {
            s_stats.img_valid++;
        } else if (v == ESP_ERR_INVALID_CRC) {
            s_stats.img_invalid++;
            if (cardfs_remove(op->target) != ESP_OK) {
                fail("delete the invalid image");
            }
        } else {
            fail("cardfs_verify: %s", esp_err_to_name(v));
        }
        return;
    }
    if (small_is_new(op, before, after)) {
        s_stats.new_++;
    } else {
        s_stats.old++;
    }
}

// Mounted after a cut and a recovery that returned rec: checks the state, then runs the operation to completion and
// checks again. A recovery or a run may fail only where a directory holds stale bytes past its end marker, and then
// the state must still be old.
static void check_recovered(const op_t *op, const tree_t *before, esp_err_t rec)
{
    static tree_t after; // too big for the main task's stack
    fatwalk_t w;
    check_volume(&w);
    const bool hidden = w.hidden_dir_clusters > 0;
    s_stats.hidden += hidden;
    if (rec != ESP_OK && !hidden) {
        fail("cardfs_recover: %s", esp_err_to_name(rec));
    }
    s_stats.lost += w.lost > 0;
    s_stats.fat_differ += w.fat_sectors_differ > 0;
    s_stats.fsinfo_stale += w.fsinfo_free != 0xFFFFFFFF && w.fsinfo_free != w.free;

    scan(&after);
    check_target(op, before, &after);
    scan(&after); // an invalid image may have been deleted
    compare(before, &after, op, rec != ESP_OK);
    check_volume(&w);

    const esp_err_t res = run_op(op);
    if (res == ESP_FAIL && hidden) {
        // Refused, loudly, and its record (if it got that far) left for the next mount: nothing else changed.
        s_stats.refused++;
        scan(&after);
        compare(before, &after, op, true);
        if (!op->image) {
            small_is_new(op, before, &after);
        }
        check_volume(&w);
        return;
    }
    if (res != ESP_OK && !(op->image && res == ESP_ERR_NOT_ALLOWED)) {
        fail("the operation again: %s", esp_err_to_name(res));
    }
    scan(&after);
    compare(before, &after, op, false);
    const entry_t *now = find(&after, op->target);
    if (!now || now->size != op->len || memcmp(now->sha, op->sha, 32) != 0) {
        fail("%s is not new after the operation ran again (%s, %ld bytes)", op->target, esp_err_to_name(res),
             now ? (long)now->size : -1L);
    }
    check_volume(&w);
}

// From the snapshot, runs op with the power cut after `whole` sector writes (tear: the next lands torn). Leaves
// the disk as the cut left it, powered on and unmounted.
static void cut_op(const op_t *op, uint32_t whole, bool tear)
{
    ramdisk_restore();
    mount();
    ramdisk_cut_after(whole, tear);
    run_op(op);
    unmount();
    ramdisk_power_on();
}

static void recover_and_check(const op_t *op, const tree_t *before)
{
    mount();
    const esp_err_t res = cardfs_recover();
    check_recovered(op, before, res);
    unmount();
}

// Cuts the recovery of the state cut_op(op, whole, tear) leaves at each of its writes, recovers again, checks.
static void cut_recovery(const op_t *op, const tree_t *before, uint32_t whole, bool tear)
{
    cut_op(op, whole, tear);
    mount();
    ramdisk_reset_count();
    cardfs_recover();
    const uint32_t writes = ramdisk_writes();
    unmount();
    for (uint32_t m = 0; m <= writes; m++) {
        for (int torn = 0; torn <= (m < writes); torn++) {
            snprintf(s_where, sizeof(s_where), "%s, cut after %lu writes%s, recovery cut after %lu%s", op->name,
                     (unsigned long)whole, tear ? " and a torn one" : "", (unsigned long)m,
                     torn ? " and a torn one" : "");
            cut_op(op, whole, tear);
            mount();
            ramdisk_cut_after(m, torn);
            cardfs_recover();
            unmount();
            ramdisk_power_on();
            recover_and_check(op, before);
            s_stats.recovery_cuts++;
        }
    }
}

static void build_card(const op_t *op)
{
    unmount();
    ramdisk_power_on();
    ramdisk_fill(0x5eed);
    MKFS_PARM opt = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = RAMDISK_SECTOR};
    static uint8_t work[4096];
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, work, sizeof(work)));
    mount();
    // A person's files, before and around ours, so our chains have neighbours in the FAT and the root directory.
    plant("notes.txt", 300, 1);
    TEST_ASSERT_EQUAL(0, mkdir(ROOT "/DCIM", 0755));
    plant("DCIM/IMG_0001.JPG", 20000, 2);
    plant("A long file name.txt", 5000, 3);
    if (op->card == CARD_OLD_FILES) {
        static const char wifi[] = "Old network\noldpassword1\n";
        static const char sleep[] =
            "{\"t\":\"sleep\",\"bg\":0,\"fg\":16777215,\"show\":[\"clock\"],\"spinner\":\"arc\"}";
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", wifi, strlen(wifi)));
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.json", sleep, strlen(sleep)));
        static uint8_t old_image[65536];
        memset(old_image, 0x5a, sizeof(old_image));
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new("watchdeck/sleep-00112233445566778899aabbccddeeff.rgb565", old_image,
                                                   sizeof(old_image)));
        plant("after.txt", 3000, 4);
    } else if (op->card == CARD_EMPTY_WATCHDECK) {
        TEST_ASSERT_EQUAL(0, mkdir(ROOT "/watchdeck", 0755)); // made by a person, or by a firmware without records
    }
    if (op->fill) {
        fill_dir(op->fill, op->fill_left);
    }
    unmount();
    ramdisk_snapshot();
}

static void run_cuts(op_t *op, bool cut_recoveries)
{
    memset(&s_stats, 0, sizeof(s_stats));
    sha256(op->data, op->len, op->sha);
    build_card(op);

    ramdisk_restore();
    mount();
    static tree_t before; // too big for the main task's stack
    scan(&before);
    ramdisk_reset_count();
    snprintf(s_where, sizeof(s_where), "%s, no cut", op->name);
    TEST_ASSERT_EQUAL(ESP_OK, run_op(op));
    const uint32_t writes = ramdisk_writes();
    TEST_ASSERT_TRUE(writes > 0);
    s_stats.writes = writes;
    unmount();

    for (uint32_t whole = 0; whole <= writes; whole++) {
        for (int torn = 0; torn <= (whole < writes); torn++) {
            snprintf(s_where, sizeof(s_where), "%s, cut after %lu writes%s", op->name, (unsigned long)whole,
                     torn ? " and a torn one" : "");
            cut_op(op, whole, torn);
            recover_and_check(op, &before);
            if (torn) {
                s_stats.cuts_torn++;
            } else {
                s_stats.cuts_whole++;
            }
            if (cut_recoveries) {
                cut_recovery(op, &before, whole, torn);
            }
        }
    }

    printf("%s: %lu sector writes; %lu cut points (%lu whole, %lu with a torn sector), %lu recovery cut points\n",
           op->name, (unsigned long)writes, (unsigned long)(s_stats.cuts_whole + s_stats.cuts_torn),
           (unsigned long)s_stats.cuts_whole, (unsigned long)s_stats.cuts_torn, (unsigned long)s_stats.recovery_cuts);
    if (op->image) {
        printf("  image after recovery: %lu absent, %lu valid, %lu detected invalid and deleted\n",
               (unsigned long)s_stats.img_absent, (unsigned long)s_stats.img_valid, (unsigned long)s_stats.img_invalid);
    } else {
        printf("  file after recovery: %lu old, %lu new\n", (unsigned long)s_stats.old, (unsigned long)s_stats.new_);
    }
    printf("  recoveries that left: lost clusters %lu, FAT copies differing %lu, a stale FSINFO free count %lu,\n"
           "  stale bytes in a grown directory %lu (and the operation then refused %lu)\n",
           (unsigned long)s_stats.lost, (unsigned long)s_stats.fat_differ, (unsigned long)s_stats.fsinfo_stale,
           (unsigned long)s_stats.hidden, (unsigned long)s_stats.refused);
    TEST_ASSERT_TRUE(s_stats.cuts_whole > 0 && s_stats.cuts_torn > 0);
    TEST_ASSERT_TRUE(!cut_recoveries || s_stats.recovery_cuts > 0);
    // Only a card built to make FatFs grow a directory may end up with stale bytes in one, and it must.
    if (op->grows) {
        TEST_ASSERT_TRUE_MESSAGE(s_stats.hidden > 0, "no cut point hit the directory growth");
    } else {
        TEST_ASSERT_EQUAL_MESSAGE(0, s_stats.hidden, "a directory grew");
    }
}

static const char NEW_WIFI[] = "A new network name\na-longer-new-password-for-it\n";
static char s_sleep_json[1600];
static size_t s_sleep_len;
static uint8_t s_image[IMAGE_LEN];
static char s_image_name[96];

void test_cuts_first_wifi_txt(void)
{
    static const char wifi[] = "Home\nsecret12\n";
    op_t op = {.name = "first wifi.txt", .target = "wifi.txt", .data = (const uint8_t *)wifi, .len = strlen(wifi)};
    run_cuts(&op, true);
}

// watchdeck/ needs two root entries and the root's last cluster has one, so FatFs grows the root.
void test_cuts_first_wifi_txt_into_a_nearly_full_root(void)
{
    static const char wifi[] = "Home\nsecret12\n";
    op_t op = {.name = "first wifi.txt, root nearly full",
               .fill = "",
               .fill_left = 1,
               .grows = true,
               .target = "wifi.txt",
               .data = (const uint8_t *)wifi,
               .len = strlen(wifi)};
    run_cuts(&op, true);
}

void test_cuts_wifi_txt_replace(void)
{
    op_t op = {.name = "wifi.txt replace",
               .card = CARD_OLD_FILES,
               .target = "wifi.txt",
               .data = (const uint8_t *)NEW_WIFI,
               .len = strlen(NEW_WIFI)};
    run_cuts(&op, true);
}

// The record's entry exists from an earlier write, so a full watchdeck/ never has to grow.
void test_cuts_wifi_txt_replace_with_a_full_watchdeck(void)
{
    op_t op = {.name = "wifi.txt replace, watchdeck full",
               .card = CARD_OLD_FILES,
               .fill = "watchdeck",
               .target = "wifi.txt",
               .data = (const uint8_t *)NEW_WIFI,
               .len = strlen(NEW_WIFI)};
    run_cuts(&op, true);
}

void test_cuts_sleep_json_replace(void)
{
    op_t op = {.name = "sleep.json replace",
               .card = CARD_OLD_FILES,
               .target = "watchdeck/sleep.json",
               .data = (const uint8_t *)s_sleep_json,
               .len = s_sleep_len};
    run_cuts(&op, true);
}

// sleep.new takes one of the two free entries and sleep.json needs two, so FatFs grows watchdeck/.
void test_cuts_first_sleep_json_into_a_nearly_full_watchdeck(void)
{
    op_t op = {.name = "first sleep.json, watchdeck nearly full",
               .card = CARD_EMPTY_WATCHDECK,
               .fill = "watchdeck",
               .fill_left = 2,
               .grows = true,
               .target = "watchdeck/sleep.json",
               .data = (const uint8_t *)s_sleep_json,
               .len = s_sleep_len};
    run_cuts(&op, true);
}

void test_cuts_1_mib_image(void)
{
    op_t op = {.name = "1 MiB image",
               .card = CARD_OLD_FILES,
               .target = s_image_name,
               .data = s_image,
               .len = sizeof(s_image),
               .image = true};
    run_cuts(&op, false);
}

// The image's name needs five entries and watchdeck/'s last cluster has four, so FatFs grows it.
void test_cuts_1_mib_image_into_a_nearly_full_watchdeck(void)
{
    op_t op = {.name = "1 MiB image, watchdeck nearly full",
               .card = CARD_OLD_FILES,
               .fill = "watchdeck",
               .fill_left = 4,
               .grows = true,
               .target = s_image_name,
               .data = s_image,
               .len = sizeof(s_image),
               .image = true};
    run_cuts(&op, false);
}

static uint32_t first_cluster(const char *rel)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", s_drive, rel);
    FIL f = {0}; // IDF's f_open allocates the buffer only when fp->buf is NULL
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, path, FA_READ));
    const uint32_t cl = f.obj.sclust;
    f_close(&f);
    return cl;
}

// Writes a sector through the RAM disk, so a restore undoes it.
static void poke(uint32_t lba, uint32_t offset, const void *data, size_t len)
{
    uint8_t buf[RAMDISK_SECTOR];
    memcpy(buf, ramdisk_sector(lba), sizeof(buf));
    memcpy(buf + offset, data, len);
    TEST_ASSERT_EQUAL(RES_OK, ramdisk_impl.write(0, buf, lba, 1));
}

static void set_fat(uint32_t cluster, uint32_t value)
{
    const uint8_t *bs = ramdisk_sector(0);
    const uint8_t le[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
    poke((bs[14] | bs[15] << 8) + cluster / 128, cluster % 128 * 4, le, 4);
}

// The checker itself: it must see what the cut test relies on it to see.
void test_fatwalk_finds_a_cross_link_a_free_cluster_and_stale_directory_bytes(void)
{
    op_t op = {.name = "fatwalk", .card = CARD_FRESH};
    build_card(&op);
    ramdisk_restore();
    mount();
    const uint32_t photo = first_cluster("DCIM/IMG_0001.JPG");
    const uint32_t doc = first_cluster("A long file name.txt");
    unmount();
    fatwalk_t w;
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(5, w.files + w.dirs); // root, DCIM and three files
    TEST_ASSERT_EQUAL(0, w.lost);
    TEST_ASSERT_EQUAL(0, w.hidden_dir_clusters);

    set_fat(doc, photo); // the document's first cluster runs on into the photo, walked before it
    TEST_ASSERT_FALSE(fatwalk(&w));
    TEST_ASSERT_NOT_NULL(strstr(w.problem, "two chains"));

    ramdisk_restore();
    set_fat(photo, 0);
    TEST_ASSERT_FALSE(fatwalk(&w));
    TEST_ASSERT_NOT_NULL(strstr(w.problem, "is free"));

    ramdisk_restore();
    set_fat(doc + 1, 0); // the document's second cluster: its chain is one cluster short
    TEST_ASSERT_FALSE(fatwalk(&w));

    ramdisk_restore();
    set_fat(60000, 0x0FFFFFFF); // allocated and in no chain
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(1, w.lost);

    ramdisk_restore();
    const uint8_t *bs = ramdisk_sector(0);
    const uint32_t root = (bs[14] | bs[15] << 8) + bs[16] * le32(bs + 36); // the root is the first data cluster
    poke(root, RAMDISK_SECTOR - 32, "STALE   BIN", 11);                    // past the end marker
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(1, w.hidden_dir_clusters);
}

static void make_data(void)
{
    // About 1.5 KB, so the new sleep.json spans three clusters where the old one had one.
    size_t n = snprintf(s_sleep_json, sizeof(s_sleep_json), "{\"t\":\"sleep\",\"text\":\"");
    while (n < sizeof(s_sleep_json) - 100) {
        n += snprintf(s_sleep_json + n, sizeof(s_sleep_json) - n, "Back soon %03u. ", (unsigned)n);
    }
    n += snprintf(s_sleep_json + n, sizeof(s_sleep_json) - n, "\",\"offline_after_s\":10}");
    s_sleep_len = n;

    uint32_t x = 2463534242u;
    for (size_t i = 0; i < sizeof(s_image); i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_image[i] = (uint8_t)x;
    }
    uint8_t sha[32];
    sha256(s_image, sizeof(s_image), sha);
    int len = snprintf(s_image_name, sizeof(s_image_name), "watchdeck/sleep-");
    for (int i = 0; i < 16; i++) {
        len += snprintf(s_image_name + len, sizeof(s_image_name) - len, "%02x", sha[i]);
    }
    snprintf(s_image_name + len, sizeof(s_image_name) - len, ".rgb565");
}

void app_main(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("psa_crypto_init failed\n");
        exit(1);
    }
    ramdisk_create(DISK_SECTORS);
    BYTE pdrv;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) {
        printf("no free FatFs drive\n");
        exit(1);
    }
    ff_diskio_register(pdrv, &ramdisk_impl);
    s_drive[0] = (char)('0' + pdrv);
    s_drive[1] = ':';
    // The board's settings: the same VFS layer and file count as sdcard.c.
    const esp_vfs_fat_conf_t conf = {.base_path = ROOT, .fat_drive = s_drive, .max_files = 4};
    if (esp_vfs_fat_register(&conf, &s_fs) != ESP_OK) {
        printf("esp_vfs_fat_register failed\n");
        exit(1);
    }
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_init(ROOT));
    cardfs_set_writable(true);
    esp_log_level_set("cardfs", ESP_LOG_NONE); // thousands of recoveries
    make_data();

    UNITY_BEGIN();
    RUN_TEST(test_fatwalk_finds_a_cross_link_a_free_cluster_and_stale_directory_bytes);
    RUN_TEST(test_cuts_first_wifi_txt);
    RUN_TEST(test_cuts_first_wifi_txt_into_a_nearly_full_root);
    RUN_TEST(test_cuts_wifi_txt_replace);
    RUN_TEST(test_cuts_wifi_txt_replace_with_a_full_watchdeck);
    RUN_TEST(test_cuts_sleep_json_replace);
    RUN_TEST(test_cuts_first_sleep_json_into_a_nearly_full_watchdeck);
    RUN_TEST(test_cuts_1_mib_image);
    RUN_TEST(test_cuts_1_mib_image_into_a_nearly_full_watchdeck);
    // Returning from app_main does not end a linux-target process, so the failure count becomes the exit status.
    exit(UNITY_END());
}
