// The FatFs power-cut test for cardfs.c: IDF's FatFs and VFS over a RAM disk that cuts the power at every sector
// write of an operation, landing that write whole, torn with its first half new, or torn with its last half new,
// then remounts, recovers and checks the volume. For small files it also cuts the recovery at every one of its
// writes and recovers again. It runs on FAT32 with 512-byte and with 4 KiB clusters.
//
// After each recovery: the operation's file is old or new (an image absent, valid, or detected invalid and
// deleted; a removed file present and unchanged, or absent), no intent record holds anything, every other file and
// directory is unchanged, fatwalk finds no cross-linked or broken chain, and no directory has grown or holds bytes
// past its end marker. Then the operation runs again to completion and the same checks hold for the new state.
//
// Growing a directory isn't cut-safe in FatFs (dir_next links the new cluster, and dir_clear flushes that link
// before it zeroes the cluster), so cardfs checks first that the directory has room (cardfs_fat.c). The "nearly
// full" cards have one entry too few: the operation must be refused with no write at all, and the same creates
// done straight through FatFs must grow a directory, so the check refuses exactly when FatFs would grow. The
// "exactly enough" cards and the hole card have just enough room, and must never grow. Nor may a name's entries
// cross a sector boundary: FatFs writes them sector by sector, so a cut between leaves orphan long-name entries.
// The "across a sector" cards must be refused, and the same creates straight through FatFs, cut at each write,
// must leave an orphan at some cut. No check anywhere may find one.
//
// mount() is what G2 does after every mount: reset FatFs's FSINFO hints, cardfs_init, cardfs_set_writable(true).
// A failed card access taints cardfs until then.
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
#include "test_format.h"
#include "unity.h"

#define ROOT "/card"
#define FAT32_CLUSTERS 66000 // FAT32 needs over 65,525 clusters
#define TREE_MAX 300
#define IMAGE_LEN (1024 * 1024)
#define OLD_IMAGE "watchdeck/00112233.img"
#define LONG_NAME "watchdeck/a-long-name.img" // 16 characters: two long-name entries and the 8.3 one

static FATFS *s_fs;
static char s_drive[3];
static const cardfs_volume_t s_vol = {
    .prepare = cardfs_fat_prepare, .room = cardfs_fat_room, .space = cardfs_fat_space, .ctx = s_drive};

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
typedef enum { OP_SMALL, OP_IMAGE, OP_REMOVE } kind_t;

typedef struct {
    const char *name;
    card_t card;
    uint32_t cluster; // bytes per cluster; 0: 512
    const char *fill; // NULL, "" (the root) or "watchdeck": filled until its last cluster has fill_left free entries
    int fill_left;
    int hole;           // then that many of the filler files, all in a row, are deleted again
    kind_t kind;        // write_small, write_new (checked with cardfs_verify), or remove
    const char *target; // relative to ROOT
    const uint8_t *data;
    size_t len;
    bool refused;     // the card has no room: CARDFS_ERR_DIR_FULL, nothing written
    bool straddle;    // refused because a name's entries would cross a sector boundary
    bool exact;       // the operation fills the last free entry of fill's last cluster
    bool count_first; // a free-space count (f_getfree) runs first, as the Storage tab's does
    uint8_t sha[32];
} op_t;

// Totals over every cut point of one operation.
typedef struct {
    uint32_t writes, cuts[3], recovery_cuts;
    uint32_t old, new_, img_absent, img_valid, img_invalid;
    uint32_t lost, fat_differ, fsinfo_stale;
} stats_t;

static stats_t s_stats;
static char s_where[200];
static uint32_t s_extra_dir_clusters; // directory clusters past each directory's first, on the snapshot

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
    s_fs->free_clst = s_fs->last_clst = 0xFFFFFFFF; // as cardfs_fat_prepare does
    esp_err_t err = cardfs_init(ROOT, &s_vol);
    if (err == ESP_OK) {
        err = cardfs_set_writable(true);
    }
    if (err != ESP_OK) {
        fail("cardfs after mount: %s", esp_err_to_name(err));
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

// Free entries (first byte 0x00) in the last cluster of the directory at cluster `start`, read from the disk.
static int free_slots(uint32_t start)
{
    const uint8_t *bs = ramdisk_sector(0);
    const uint32_t spc = bs[13];
    const uint32_t fatbase = bs[14] | bs[15] << 8;
    const uint32_t database = fatbase + bs[16] * le32(bs + 36);
    uint32_t c = start;
    for (uint32_t next; (next = le32(ramdisk_sector(fatbase + c / 128) + c % 128 * 4) & 0x0FFFFFFF) < 0x0FFFFFF8;) {
        c = next;
    }
    int n = 0;
    for (uint32_t s = 0; s < spc; s++) {
        const uint8_t *d = ramdisk_sector(database + (c - 2) * spc + s);
        for (int e = 0; e < RAMDISK_SECTOR / 32; e++) {
            n += d[e * 32] == 0;
        }
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
    FF_DIR d = {0};
    TEST_ASSERT_EQUAL(FR_OK, f_opendir(&d, path));
    const uint32_t cl = d.obj.sclust;
    f_closedir(&d);
    return cl;
}

// Plants empty files (one entry each, no cluster) in dir until its last cluster has `left` free entries, then
// deletes the first `hole` of them again.
static void fill_dir(const char *dir, int left, int hole)
{
    const uint32_t cl = dir_cluster(dir);
    char rel[64];
    int i = 0;
    for (; free_slots(cl) != left; i++) {
        TEST_ASSERT_TRUE(i < 1000);
        snprintf(rel, sizeof(rel), "%s%sF%03d", dir, dir[0] ? "/" : "", i);
        plant(rel, 0, 0);
    }
    TEST_ASSERT_TRUE(hole <= i);
    for (int k = 0; k < hole; k++) {
        char path[96];
        snprintf(path, sizeof(path), ROOT "/%s%sF%03d", dir, dir[0] ? "/" : "", k);
        TEST_ASSERT_EQUAL(0, unlink(path));
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
// be empty. Nothing else appeared.
static void compare(const tree_t *before, const tree_t *after, const char *target)
{
    for (int i = 0; i < before->n; i++) {
        const entry_t *b = &before->e[i];
        if (target && strcmp(b->path, target) == 0) {
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
        if (is_record(a->path) && a->size != 0) {
            fail("%s holds %u bytes after recovery", a->path, (unsigned)a->size);
        }
        if (find(before, a->path) || (target && strcmp(a->path, target) == 0)) {
            continue;
        }
        if (!(target && (is_record(a->path) || (strcmp(a->path, "watchdeck") == 0 && a->is_dir)))) {
            fail("%s appeared", a->path);
        }
    }
}

static esp_err_t run_op(const op_t *op)
{
    if (op->count_first) {
        uint64_t total, free_bytes;
        const esp_err_t err = cardfs_free_space(&total, &free_bytes);
        if (err != ESP_OK) {
            return err;
        }
    }
    switch (op->kind) {
    case OP_IMAGE:
        return cardfs_write_new(op->target, op->data, op->len);
    case OP_REMOVE:
        return cardfs_remove(op->target);
    default:
        return cardfs_write_small(op->target, op->data, op->len);
    }
}

// No broken chain, no directory grown, nothing past an end marker, no orphan long-name entry.
static void check_volume(fatwalk_t *w)
{
    if (!fatwalk(w)) {
        fail("fatwalk: %s", w->problem);
    }
    if (w->hidden_dir_clusters > 0) {
        fail("%lu directory clusters hold bytes past the end marker", (unsigned long)w->hidden_dir_clusters);
    }
    if (w->orphan_lfn > 0) {
        fail("%lu orphan long-name entries", (unsigned long)w->orphan_lfn);
    }
    if (w->dir_clusters - w->dirs != s_extra_dir_clusters) {
        fail("a directory grew: %lu clusters past the first, %lu before", (unsigned long)(w->dir_clusters - w->dirs),
             (unsigned long)s_extra_dir_clusters);
    }
    DWORD free_clusters;
    FATFS *fs;
    const FRESULT res = f_getfree(s_drive, &free_clusters, &fs);
    if (res != FR_OK) {
        fail("f_getfree: FRESULT %d", res);
    }
}

// True when the target is new (written; for a remove, absent), false when it's old, a failure when it's neither.
static bool target_is_new(const op_t *op, const tree_t *before, const tree_t *after)
{
    const entry_t *now = find(after, op->target);
    const entry_t *old = find(before, op->target);
    if (op->kind == OP_REMOVE) {
        if (now && !(old && same_entry(now, old))) {
            fail("%s is neither removed nor as it was (%u bytes)", op->target, (unsigned)now->size);
        }
        return !now;
    }
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

// The target after a recovery: an image absent, valid, or invalid and then deleted; anything else old or new.
static void check_target(const op_t *op, const tree_t *before, const tree_t *after)
{
    if (op->kind == OP_IMAGE) {
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
    if (target_is_new(op, before, after)) {
        s_stats.new_++;
    } else {
        s_stats.old++;
    }
}

// Mounted after a cut and a recovery that returned rec: checks the state, then runs the operation to completion and
// checks again.
static void check_recovered(const op_t *op, const tree_t *before, esp_err_t rec)
{
    static tree_t after; // too big for the main task's stack
    fatwalk_t w;
    check_volume(&w);
    if (rec != ESP_OK) {
        fail("cardfs_recover: %s", esp_err_to_name(rec));
    }
    s_stats.lost += w.lost > 0;
    s_stats.fat_differ += w.fat_sectors_differ > 0;
    s_stats.fsinfo_stale += w.fsinfo_free != 0xFFFFFFFF && w.fsinfo_free != w.free;

    scan(&after);
    check_target(op, before, &after);
    scan(&after); // an invalid image may have been deleted
    compare(before, &after, op->target);
    check_volume(&w);

    const esp_err_t res = run_op(op);
    const bool done = res == ESP_OK || (op->kind == OP_IMAGE && res == ESP_ERR_NOT_ALLOWED) ||
                      (op->kind == OP_REMOVE && res == ESP_ERR_NOT_FOUND);
    if (!done) {
        fail("the operation again: %s", esp_err_to_name(res));
    }
    scan(&after);
    compare(before, &after, op->target);
    if (!target_is_new(op, before, &after)) {
        fail("%s is not new after the operation ran again (%s)", op->target, esp_err_to_name(res));
    }
    check_volume(&w);
}

static const char *const TEAR_NAME[] = {"", " and a torn one (first half new)", " and a torn one (last half new)"};

// From the snapshot, runs op with the power cut after `whole` sector writes and the next landing as tear says.
// Leaves the disk as the cut left it, powered on and unmounted.
static void cut_op(const op_t *op, uint32_t whole, ramdisk_tear_t tear)
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
static void cut_recovery(const op_t *op, const tree_t *before, uint32_t whole, ramdisk_tear_t tear)
{
    cut_op(op, whole, tear);
    mount();
    ramdisk_reset_count();
    cardfs_recover();
    const uint32_t writes = ramdisk_writes();
    unmount();
    for (uint32_t m = 0; m <= writes; m++) {
        for (int t = RAMDISK_WHOLE; t <= (m < writes ? RAMDISK_TEAR_TAIL : RAMDISK_WHOLE); t++) {
            snprintf(s_where, sizeof(s_where), "%s, cut after %lu writes%s, recovery cut after %lu%s", op->name,
                     (unsigned long)whole, TEAR_NAME[tear], (unsigned long)m, TEAR_NAME[t]);
            cut_op(op, whole, tear);
            mount();
            ramdisk_cut_after(m, (ramdisk_tear_t)t);
            cardfs_recover();
            unmount();
            ramdisk_power_on();
            recover_and_check(op, before);
            s_stats.recovery_cuts++;
        }
    }
}

static uint32_t cluster_of(const op_t *op)
{
    return op->cluster ? op->cluster : RAMDISK_SECTOR;
}

static void build_card(const op_t *op)
{
    unmount();
    const uint32_t spc = cluster_of(op) / RAMDISK_SECTOR;
    ramdisk_create(FAT32_CLUSTERS * spc + 4096, 0x5eed);
    MKFS_PARM opt = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = cluster_of(op)};
    static uint8_t work[4096];
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, work, sizeof(work)));
    mount();
    TEST_ASSERT_EQUAL(spc, s_fs->csize);
    TEST_ASSERT_EQUAL(FS_FAT32, s_fs->fs_type);
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
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("watchdeck/sleep.cfg", sleep, strlen(sleep)));
        static uint8_t old_image[65536];
        memset(old_image, 0x5a, sizeof(old_image));
        TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_new(OLD_IMAGE, old_image, sizeof(old_image)));
        plant("after.txt", 3000, 4);
    } else if (op->card == CARD_EMPTY_WATCHDECK) {
        TEST_ASSERT_EQUAL(0, mkdir(ROOT "/watchdeck", 0755)); // made by a person, or by a firmware without records
    }
    if (op->fill) {
        fill_dir(op->fill, op->fill_left, op->hole);
    }
    unmount();
    ramdisk_snapshot();
    fatwalk_t w;
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(0, w.hidden_dir_clusters);
    TEST_ASSERT_EQUAL(0, w.orphan_lfn);
    s_extra_dir_clusters = w.dir_clusters - w.dirs;
}

static void create_if_absent(const char *rel)
{
    char path[128];
    snprintf(path, sizeof(path), ROOT "/%s", rel);
    struct stat st;
    if (stat(path, &st) != 0) {
        const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            close(fd);
        }
    }
}

// The creates a refused operation would have made, in its order, done straight through FatFs. Errors are ignored:
// a cut may stop them.
static void raw_creates(const op_t *op)
{
    struct stat st;
    if (stat(ROOT "/watchdeck", &st) != 0) {
        mkdir(ROOT "/watchdeck", 0755);
    }
    if (op->kind == OP_SMALL) {
        create_if_absent(strcmp(op->target, "wifi.txt") == 0 ? "watchdeck/wifi.new" : "watchdeck/sleep.new");
    }
    create_if_absent(op->target);
}

// Mounted: the raw creates must grow a directory.
static void check_growth_was_coming(const op_t *op)
{
    raw_creates(op);
    unmount();
    fatwalk_t w;
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_TRUE_MESSAGE(w.dir_clusters - w.dirs > s_extra_dir_clusters, "refused, but FatFs had room");
    mount();
}

// The raw creates, cut at each of their writes, must leave an orphan long-name entry at some cut. Returns the cuts
// that did.
static uint32_t check_orphan_was_coming(const op_t *op)
{
    ramdisk_restore();
    mount();
    ramdisk_reset_count();
    raw_creates(op);
    const uint32_t writes = ramdisk_writes();
    unmount();
    uint32_t orphans = 0;
    for (uint32_t m = 0; m < writes; m++) {
        ramdisk_restore();
        mount();
        ramdisk_cut_after(m, RAMDISK_WHOLE);
        raw_creates(op);
        unmount();
        ramdisk_power_on();
        fatwalk_t w;
        TEST_ASSERT_TRUE(fatwalk(&w));
        orphans += w.orphan_lfn > 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(orphans > 0, "refused, but no cut of FatFs's own creates leaves an orphan");
    return orphans;
}

static void print_stats(const op_t *op)
{
    printf("%s: %lu sector writes; %lu cut points (%lu whole, %lu torn first half new, %lu torn last half new), "
           "%lu recovery cut points\n",
           op->name, (unsigned long)s_stats.writes,
           (unsigned long)(s_stats.cuts[0] + s_stats.cuts[1] + s_stats.cuts[2]), (unsigned long)s_stats.cuts[0],
           (unsigned long)s_stats.cuts[1], (unsigned long)s_stats.cuts[2], (unsigned long)s_stats.recovery_cuts);
    if (op->kind == OP_IMAGE) {
        printf("  image after recovery: %lu absent, %lu valid, %lu detected invalid and deleted\n",
               (unsigned long)s_stats.img_absent, (unsigned long)s_stats.img_valid, (unsigned long)s_stats.img_invalid);
    } else {
        printf("  %s after recovery: %lu old, %lu new\n", op->target, (unsigned long)s_stats.old,
               (unsigned long)s_stats.new_);
    }
    printf("  recoveries that left: lost clusters %lu, FAT copies differing %lu, a stale FSINFO free count %lu\n",
           (unsigned long)s_stats.lost, (unsigned long)s_stats.fat_differ, (unsigned long)s_stats.fsinfo_stale);
}

static void run_cuts(op_t *op, bool cut_recoveries)
{
    memset(&s_stats, 0, sizeof(s_stats));
    if (op->data) {
        sha256(op->data, op->len, op->sha);
    }
    build_card(op);

    ramdisk_restore();
    mount();
    static tree_t before; // too big for the main task's stack
    static tree_t after;
    scan(&before);
    ramdisk_reset_count();
    snprintf(s_where, sizeof(s_where), "%s, no cut", op->name);
    const esp_err_t res = run_op(op);
    const uint32_t writes = ramdisk_writes();
    s_stats.writes = writes;
    if (op->refused) {
        TEST_ASSERT_EQUAL_HEX(CARDFS_ERR_DIR_FULL, res);
        TEST_ASSERT_EQUAL(0, writes);
        scan(&after);
        compare(&before, &after, NULL);
        fatwalk_t w;
        check_volume(&w);
        if (op->straddle) {
            unmount();
            const uint32_t orphans = check_orphan_was_coming(op);
            printf("%s: refused with no sector write; the same creates through FatFs leave an orphan long-name entry "
                   "at %lu cut points\n",
                   op->name, (unsigned long)orphans);
            return;
        }
        check_growth_was_coming(op);
        unmount();
        printf("%s: refused with no sector write; the same creates through FatFs grow a directory\n", op->name);
        return;
    }
    TEST_ASSERT_EQUAL_HEX(ESP_OK, res);
    TEST_ASSERT_TRUE(writes > 0);
    if (op->exact) {
        TEST_ASSERT_EQUAL_MESSAGE(0, free_slots(dir_cluster(op->fill)), "room was left over");
    }
    unmount();

    for (uint32_t whole = 0; whole <= writes; whole++) {
        for (int t = RAMDISK_WHOLE; t <= (whole < writes ? RAMDISK_TEAR_TAIL : RAMDISK_WHOLE); t++) {
            snprintf(s_where, sizeof(s_where), "%s, cut after %lu writes%s", op->name, (unsigned long)whole,
                     TEAR_NAME[t]);
            cut_op(op, whole, (ramdisk_tear_t)t);
            recover_and_check(op, &before);
            s_stats.cuts[t]++;
            if (cut_recoveries) {
                cut_recovery(op, &before, whole, (ramdisk_tear_t)t);
            }
        }
    }
    print_stats(op);
    TEST_ASSERT_TRUE(s_stats.cuts[0] > 0 && s_stats.cuts[1] > 0 && s_stats.cuts[2] > 0);
    TEST_ASSERT_TRUE(!cut_recoveries || s_stats.recovery_cuts > 0);
}

static const char NEW_WIFI[] = "A new network name\na-longer-new-password-for-it\n";
static const char HOME_WIFI[] = "Home\nsecret12\n";
static char s_sleep_cfg[1600];
static size_t s_sleep_len;
static uint8_t s_image[IMAGE_LEN];
static char s_image_name[32];

#define SMALL_OP(...) ((op_t){.kind = OP_SMALL, __VA_ARGS__})
#define WIFI(text) .target = "wifi.txt", .data = (const uint8_t *)(text), .len = sizeof(text) - 1
#define SLEEP .target = "watchdeck/sleep.cfg", .data = (const uint8_t *)s_sleep_cfg, .len = s_sleep_len
#define IMAGE .kind = OP_IMAGE, .target = s_image_name, .data = s_image, .len = sizeof(s_image)

void test_cuts_first_wifi_txt(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt", WIFI(HOME_WIFI));
    run_cuts(&op, true);
}

// watchdeck/ takes two root entries and wifi.txt one: three free entries at the end of the root's last cluster are
// just enough.
void test_cuts_first_wifi_txt_into_a_root_with_exactly_enough_room(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, root with exactly enough room", .fill = "", .fill_left = 3,
                       .exact = true, WIFI(HOME_WIFI));
    run_cuts(&op, true);
}

void test_cuts_first_wifi_txt_into_a_nearly_full_root_is_refused(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, root nearly full", .fill = "", .fill_left = 2, .refused = true,
                       WIFI(HOME_WIFI));
    run_cuts(&op, false);
}

void test_cuts_wifi_txt_replace(void)
{
    op_t op = SMALL_OP(.name = "wifi.txt replace", .card = CARD_OLD_FILES, WIFI(NEW_WIFI));
    run_cuts(&op, true);
}

// The Storage tab's count (which makes FatFs trust its fresh free count and flag FSINFO) before the same replace.
void test_cuts_wifi_txt_replace_after_a_free_space_count(void)
{
    op_t op =
        SMALL_OP(.name = "wifi.txt replace after a count", .card = CARD_OLD_FILES, .count_first = true, WIFI(NEW_WIFI));
    run_cuts(&op, true);
}

// Every entry exists from an earlier write, so a full watchdeck/ needs no room.
void test_cuts_wifi_txt_replace_with_a_full_watchdeck(void)
{
    op_t op = SMALL_OP(.name = "wifi.txt replace, watchdeck full", .card = CARD_OLD_FILES, .fill = "watchdeck",
                       WIFI(NEW_WIFI));
    run_cuts(&op, true);
}

void test_cuts_sleep_cfg_replace(void)
{
    op_t op = SMALL_OP(.name = "sleep.cfg replace", .card = CARD_OLD_FILES, SLEEP);
    run_cuts(&op, true);
}

// sleep.new and sleep.cfg take one entry each.
void test_cuts_first_sleep_cfg_into_a_watchdeck_with_exactly_enough_room(void)
{
    op_t op = SMALL_OP(.name = "first sleep.cfg, watchdeck with exactly enough room", .card = CARD_EMPTY_WATCHDECK,
                       .fill = "watchdeck", .fill_left = 2, .exact = true, SLEEP);
    run_cuts(&op, true);
}

void test_cuts_first_sleep_cfg_into_a_nearly_full_watchdeck_is_refused(void)
{
    op_t op = SMALL_OP(.name = "first sleep.cfg, watchdeck nearly full", .card = CARD_EMPTY_WATCHDECK,
                       .fill = "watchdeck", .fill_left = 1, .refused = true, SLEEP);
    run_cuts(&op, false);
}

void test_cuts_1_mib_image(void)
{
    op_t op = {.name = "1 MiB image", .card = CARD_OLD_FILES, IMAGE};
    run_cuts(&op, false);
}

// watchdeck/'s last cluster is full, but deleting a file on a PC left a hole FatFs reuses for the image's one
// entry.
void test_cuts_1_mib_image_into_a_hole_in_a_full_watchdeck(void)
{
    op_t op = {.name = "1 MiB image, hole in a full watchdeck",
               .card = CARD_OLD_FILES,
               .fill = "watchdeck",
               .fill_left = 0,
               .hole = 1,
               IMAGE};
    run_cuts(&op, false);
}

void test_cuts_1_mib_image_into_a_full_watchdeck_is_refused(void)
{
    op_t op = {.name = "1 MiB image, watchdeck full",
               .card = CARD_OLD_FILES,
               .fill = "watchdeck",
               .fill_left = 0,
               .refused = true,
               IMAGE};
    run_cuts(&op, false);
}

static const uint8_t LONG_DATA[600] = {1, 2, 3};
#define LONG .kind = OP_IMAGE, .target = LONG_NAME, .data = LONG_DATA, .len = sizeof(LONG_DATA)

// A three-entry name within one sector: watchdeck/ filled to entry 8, so the name takes entries 8 to 10 of its
// one-sector cluster. (Entries 6 to 8 would cross the middle of the sector, where this RAM disk tears a sector,
// and a torn write would leave an orphan; cardfs only keeps names within a sector, which FatFs writes whole.)
void test_cuts_a_long_name_within_a_sector(void)
{
    op_t op = {
        .name = "a long name within a sector", .card = CARD_OLD_FILES, .fill = "watchdeck", .fill_left = 16 - 8, LONG};
    run_cuts(&op, false);
}

// 4 KiB clusters, watchdeck/ filled to entry 14: the name would take entries 14 and 15 of one sector and entry 0
// of the next.
void test_cuts_a_long_name_across_a_sector_is_refused(void)
{
    op_t op = {.name = "a long name across a sector, 4 KiB clusters",
               .card = CARD_OLD_FILES,
               .cluster = 4096,
               .fill = "watchdeck",
               .fill_left = 128 - 14,
               .refused = true,
               .straddle = true,
               LONG};
    run_cuts(&op, false);
}

// The same for watchdeck/ itself, whose name takes two entries: the root filled to entry 15.
void test_cuts_first_wifi_txt_with_watchdeck_across_a_sector_is_refused(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, watchdeck/ across a sector, 4 KiB clusters", .cluster = 4096,
                       .fill = "", .fill_left = 128 - 15, .refused = true, .straddle = true, WIFI(HOME_WIFI));
    run_cuts(&op, false);
}

void test_cuts_remove_an_image(void)
{
    op_t op = {.name = "remove an image", .card = CARD_OLD_FILES, .kind = OP_REMOVE, .target = OLD_IMAGE};
    run_cuts(&op, false);
}

// 4 KiB clusters: eight sectors to a cluster, so dir_clear and the FAT writes span several sectors.
void test_cuts_first_wifi_txt_with_4_kib_clusters(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, 4 KiB clusters", .cluster = 4096, WIFI(HOME_WIFI));
    run_cuts(&op, true);
}

void test_cuts_first_wifi_txt_into_a_root_with_exactly_enough_room_with_4_kib_clusters(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, root with exactly enough room, 4 KiB clusters", .cluster = 4096,
                       .fill = "", .fill_left = 3, .exact = true, WIFI(HOME_WIFI));
    run_cuts(&op, true);
}

void test_cuts_first_wifi_txt_into_a_nearly_full_root_with_4_kib_clusters_is_refused(void)
{
    op_t op = SMALL_OP(.name = "first wifi.txt, root nearly full, 4 KiB clusters", .cluster = 4096, .fill = "",
                       .fill_left = 2, .refused = true, WIFI(HOME_WIFI));
    run_cuts(&op, false);
}

void test_cuts_sleep_cfg_replace_with_4_kib_clusters(void)
{
    op_t op = SMALL_OP(.name = "sleep.cfg replace, 4 KiB clusters", .card = CARD_OLD_FILES, .cluster = 4096, SLEEP);
    run_cuts(&op, true);
}

void test_cuts_1_mib_image_with_4_kib_clusters(void)
{
    op_t op = {.name = "1 MiB image, 4 KiB clusters", .card = CARD_OLD_FILES, .cluster = 4096, IMAGE};
    run_cuts(&op, false);
}

// A write error with the power on, after the record of write 1 has landed, leaves wifi.txt torn and the record
// pending. The error taints cardfs, so write 2 comes after a remount, and it must apply that record before its own
// step 1 empties it, so a cut anywhere in write 2 leaves wifi.txt old, as write 1 left it, or new.
void test_cuts_retry_after_a_write_error_never_leaves_a_torn_wifi_txt(void)
{
    static const char NEWER_WIFI[] = "The newest network\nand-its-password\n";
    op_t first = SMALL_OP(.name = "wifi.txt write with an error", .card = CARD_OLD_FILES, WIFI(NEW_WIFI));
    op_t retry = SMALL_OP(.name = "wifi.txt write after the error", .card = CARD_OLD_FILES, WIFI(NEWER_WIFI));
    sha256(first.data, first.len, first.sha);
    sha256(retry.data, retry.len, retry.sha);
    build_card(&first);

    ramdisk_restore();
    mount();
    static tree_t before;
    static tree_t after;
    scan(&before);
    const entry_t *old = find(&before, "wifi.txt");
    TEST_ASSERT_NOT_NULL(old);
    ramdisk_reset_count();
    TEST_ASSERT_EQUAL(ESP_OK, run_op(&first));
    const uint32_t first_writes = ramdisk_writes();
    unmount();

    uint32_t runs = 0, failed = 0, pending = 0;
    for (uint32_t k = 0; k < first_writes; k++) {
        // The retry's own write count, with no cut.
        snprintf(s_where, sizeof(s_where), "write error after %lu writes", (unsigned long)k);
        ramdisk_restore();
        mount();
        ramdisk_fail_after(k);
        const bool write_failed = run_op(&first) != ESP_OK;
        failed += write_failed;
        ramdisk_power_on();
        if (write_failed) {
            TEST_ASSERT_EQUAL_HEX(ESP_ERR_INVALID_STATE, run_op(&retry)); // tainted until the remount
        }
        char rec_path[64] = ROOT "/watchdeck/wifi.new";
        struct stat st;
        pending += stat(rec_path, &st) == 0 && st.st_size > 0;
        unmount();
        mount();
        ramdisk_reset_count();
        TEST_ASSERT_EQUAL(ESP_OK, run_op(&retry));
        const uint32_t retry_writes = ramdisk_writes();
        unmount();
        for (uint32_t m = 0; m <= retry_writes; m++) {
            for (int t = RAMDISK_WHOLE; t <= (m < retry_writes ? RAMDISK_TEAR_TAIL : RAMDISK_WHOLE); t++) {
                snprintf(s_where, sizeof(s_where), "write error after %lu writes, retry cut after %lu%s",
                         (unsigned long)k, (unsigned long)m, TEAR_NAME[t]);
                ramdisk_restore();
                mount();
                ramdisk_fail_after(k);
                run_op(&first);
                ramdisk_power_on();
                unmount();
                mount();
                ramdisk_cut_after(m, (ramdisk_tear_t)t);
                run_op(&retry);
                unmount();
                ramdisk_power_on();
                mount();
                if (cardfs_recover() != ESP_OK) {
                    fail("cardfs_recover failed");
                }
                fatwalk_t w;
                check_volume(&w);
                scan(&after);
                compare(&before, &after, "wifi.txt");
                const entry_t *now = find(&after, "wifi.txt");
                entry_t want_first = {.size = first.len}, want_retry = {.size = retry.len};
                memcpy(want_first.sha, first.sha, 32);
                memcpy(want_retry.sha, retry.sha, 32);
                if (!now || !(same_entry(now, old) || same_entry(now, &want_first) || same_entry(now, &want_retry))) {
                    fail("wifi.txt is torn: %s, %u bytes", now ? "present" : "absent", now ? (unsigned)now->size : 0);
                }
                unmount();
                runs++;
            }
        }
    }
    printf("write error at each of %lu writes (%lu failed, %lu left a pending record), then the retry cut at each "
           "write: %lu runs\n",
           (unsigned long)first_writes, (unsigned long)failed, (unsigned long)pending, (unsigned long)runs);
    TEST_ASSERT_TRUE(pending > 0);
}

// One write fails, the next ones land: where it hits wifi.txt itself, the write tries its record once more and
// succeeds, so it returns ESP_OK with wifi.txt new, and taints cardfs all the same. With the record's hash failing
// as well, the second try can't check the record, so it fails and leaves the record for the next mount.
void test_cuts_one_failed_write_is_retried_from_the_record(void)
{
    op_t op = SMALL_OP(.name = "wifi.txt write with one failed sector", .card = CARD_OLD_FILES, WIFI(NEW_WIFI));
    sha256(op.data, op.len, op.sha);
    build_card(&op);
    ramdisk_restore();
    mount();
    static tree_t before;
    static tree_t after;
    scan(&before);
    ramdisk_reset_count();
    TEST_ASSERT_EQUAL(ESP_OK, run_op(&op));
    const uint32_t writes = ramdisk_writes();
    unmount();

    uint32_t retried = 0, failed = 0, hash_failed = 0, unreported = 0;
    for (uint32_t k = 0; k < writes; k++) {
        for (int hash = 0; hash <= 1; hash++) {
            snprintf(s_where, sizeof(s_where), "one failed write after %lu%s", (unsigned long)k,
                     hash ? ", and the record's hash fails" : "");
            ramdisk_restore();
            mount();
            ramdisk_reset_count();
            ramdisk_fail_once_after(k);
            // The write's hashes: its record's header, then the second try's check of the record.
            cardfs_test_fail_sha256(hash ? 1 : -1);
            const esp_err_t res = run_op(&op);
            cardfs_test_fail_sha256(-1);
            const bool second_try = ramdisk_writes() > writes;
            struct stat st;
            const bool pending = stat(ROOT "/watchdeck/wifi.new", &st) == 0 && st.st_size > 0;
            if (res == ESP_OK) {
                retried += second_try;
                scan(&after);
                if (!target_is_new(&op, &before, &after)) {
                    fail("ESP_OK, but wifi.txt isn't new");
                }
            } else {
                failed++;
                hash_failed += hash && pending;
            }
            const esp_err_t again = run_op(&op);
            if (res != ESP_OK || second_try) {
                TEST_ASSERT_EQUAL_HEX(ESP_ERR_INVALID_STATE, again); // tainted either way
            } else if (again != ESP_ERR_INVALID_STATE) {
                TEST_ASSERT_EQUAL_HEX(ESP_OK, again);
                // FatFs ignores a failed write of FAT 2 or FSINFO (ff.c sync_window, sync_fs): FAT 1 holds the
                // chains and the FSINFO hints are reset at mount, so nothing needs a remount.
                const uint8_t *bs = ramdisk_sector(0);
                const uint32_t fat2 = (bs[14] | bs[15] << 8) + le32(bs + 36);
                const uint32_t lba = ramdisk_failed_lba();
                if (!((lba >= fat2 && lba < fat2 + le32(bs + 36)) || lba == (uint32_t)(bs[48] | bs[49] << 8))) {
                    fail("a failed write of sector %lu went unreported", (unsigned long)lba);
                }
                unreported++;
            }
            unmount();
            mount();
            TEST_ASSERT_EQUAL(ESP_OK, cardfs_recover());
            fatwalk_t w;
            check_volume(&w);
            scan(&after);
            compare(&before, &after, "wifi.txt");
            const bool is_new = target_is_new(&op, &before, &after); // fails when wifi.txt is neither old nor new
            if (res == ESP_OK && !is_new) {
                fail("wifi.txt isn't new after the remount");
            }
            if (pending && !is_new) {
                fail("the pending record was lost");
            }
            unmount();
        }
    }
    printf("one failed write at each of %lu writes, with and without a hash failure: %lu succeeded at the second "
           "try, %lu failed (%lu of them because the second try couldn't hash the record), %lu FatFs didn't report\n",
           (unsigned long)writes, (unsigned long)retried, (unsigned long)failed, (unsigned long)hash_failed,
           (unsigned long)unreported);
    TEST_ASSERT_TRUE(retried > 0);
    TEST_ASSERT_TRUE(hash_failed > 0);
}

// The free-space count scans the allocation table afresh (cardfs_fat_prepare invalidated FatFs's hint) and writes
// nothing, and a FAT16 volume is counted too.
void test_free_space_counts_the_clusters_and_writes_nothing(void)
{
    unmount();
    ramdisk_create(FAT32_CLUSTERS + 4096, 0x5eed);
    MKFS_PARM opt = {.fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = RAMDISK_SECTOR};
    static uint8_t work[4096];
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, work, sizeof(work)));
    mount();
    plant("data.bin", 5000, 1); // 10 clusters of 512 bytes
    unmount();
    mount(); // FatFs's cached free count is gone

    ramdisk_reset_count();
    uint64_t total = 0, free_bytes = 0;
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_free_space(&total, &free_bytes));
    TEST_ASSERT_EQUAL(0, ramdisk_writes());
    const uint64_t cluster = (uint64_t)s_fs->csize * RAMDISK_SECTOR;
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(s_fs->n_fatent - 2) * cluster, total);
    TEST_ASSERT_EQUAL_UINT64(total - 11 * cluster, free_bytes); // the file's 10 clusters and the root directory's

    // A write after the count syncs FSINFO, which then holds the exact free count (not the hint prepare cleared).
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_write_small("wifi.txt", HOME_WIFI, sizeof(HOME_WIFI) - 1));
    unmount();
    fatwalk_t w;
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL_UINT32(w.free, w.fsinfo_free);

    ramdisk_create(40000, 0x5eed);
    opt.fmt = FM_FAT | FM_SFD;
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, work, sizeof(work)));
    TEST_ASSERT_EQUAL(FR_OK, f_mount(s_fs, s_drive, 1));
    TEST_ASSERT_EQUAL(FS_FAT16, s_fs->fs_type);
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_NOT_SUPPORTED, cardfs_init(ROOT, &s_vol));
    TEST_ASSERT_EQUAL(ESP_OK, cardfs_free_space(&total, &free_bytes)); // reading works on a read-only card
    TEST_ASSERT_TRUE(total > 0);
    TEST_ASSERT_EQUAL_UINT64(total, free_bytes); // a FAT16 root directory is outside the data area
    unmount();
}

// The card is FAT32. A FAT16 volume is refused at init, when writes are turned on, and by the room function.
void test_a_fat16_volume_is_refused(void)
{
    unmount();
    ramdisk_create(40000, 0x5eed);
    MKFS_PARM opt = {.fmt = FM_FAT | FM_SFD, .n_fat = 2, .au_size = RAMDISK_SECTOR};
    static uint8_t work[4096];
    TEST_ASSERT_EQUAL(FR_OK, f_mkfs(s_drive, &opt, work, sizeof(work)));
    TEST_ASSERT_EQUAL(FR_OK, f_mount(s_fs, s_drive, 1));
    TEST_ASSERT_EQUAL(FS_FAT16, s_fs->fs_type);
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_NOT_SUPPORTED, cardfs_init(ROOT, &s_vol));
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_NOT_SUPPORTED, cardfs_set_writable(true));
    TEST_ASSERT_FALSE(cardfs_writable());
    const uint8_t runs[] = {1};
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_NOT_SUPPORTED, cardfs_fat_room("", runs, 1, s_drive));
    ramdisk_reset_count();
    TEST_ASSERT_EQUAL_HEX(ESP_ERR_INVALID_STATE, cardfs_write_small("wifi.txt", HOME_WIFI, sizeof(HOME_WIFI) - 1));
    TEST_ASSERT_EQUAL(0, ramdisk_writes());
    unmount();
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

// The first slot whose first byte is 0x00 in the root's first cluster, as a sector and an offset.
static void root_marker(uint32_t *lba, uint32_t *offset)
{
    const uint8_t *bs = ramdisk_sector(0);
    const uint32_t root = (bs[14] | bs[15] << 8) + bs[16] * le32(bs + 36); // the root is the first data cluster
    for (uint32_t i = 0; i < (uint32_t)bs[13] * RAMDISK_SECTOR / 32; i++) {
        if (ramdisk_sector(root + i / 16)[i % 16 * 32] == 0) {
            *lba = root + i / 16;
            *offset = i % 16 * 32;
            return;
        }
    }
    TEST_FAIL_MESSAGE("no end marker");
}

// The checker itself: it must see what the cut test relies on it to see.
void test_fatwalk_finds_a_cross_link_a_free_cluster_stale_directory_bytes_and_an_orphan_long_name(void)
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
    TEST_ASSERT_EQUAL(0, w.orphan_lfn); // "A long file name.txt" has a valid one
    TEST_ASSERT_EQUAL(2, w.dir_clusters);

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

    uint32_t lba, offset;
    ramdisk_restore();
    root_marker(&lba, &offset);
    poke(lba + (offset + 64) / RAMDISK_SECTOR, (offset + 64) % RAMDISK_SECTOR, "STALE   BIN", 11); // past it
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(1, w.hidden_dir_clusters);

    ramdisk_restore();
    const uint8_t lfn[14] = {0x41, 'x', 0, 'y', 0, 'z', 0, 0, 0, 0xFF, 0xFF, 0x0F, 0, 0x77};
    poke(lba, offset, lfn, sizeof(lfn)); // a long-name part where the marker was, with no 8.3 entry after it
    TEST_ASSERT_TRUE(fatwalk(&w));
    TEST_ASSERT_EQUAL(1, w.orphan_lfn);
}

// An entry past the root's end marker (what a cut while FatFs grew it leaves): FatFs would place wifi.txt where
// the next lookup reads that entry as a file, so the room check refuses before any write.
void test_room_refuses_a_directory_with_an_entry_past_its_end_marker(void)
{
    op_t op = {.name = "room", .card = CARD_FRESH};
    build_card(&op);
    ramdisk_restore();
    uint32_t lba, offset;
    root_marker(&lba, &offset);
    poke(lba + (offset + 64) / RAMDISK_SECTOR, (offset + 64) % RAMDISK_SECTOR, "STALE   BIN", 11);
    mount();
    ramdisk_reset_count();
    TEST_ASSERT_EQUAL_HEX(CARDFS_ERR_DIR_FULL, cardfs_write_small("wifi.txt", HOME_WIFI, sizeof(HOME_WIFI) - 1));
    TEST_ASSERT_EQUAL_HEX(CARDFS_ERR_DIR_FULL, cardfs_write_new(s_image_name, s_image, 10)); // watchdeck/ is new
    TEST_ASSERT_EQUAL(0, ramdisk_writes());
    unmount();
}

// A watchdeck/ entry whose start cluster is 0 (FatFs then reads the root for it) or the root's own: the room
// function refuses it rather than check the root for an entry meant for watchdeck/.
void test_room_refuses_a_watchdeck_that_starts_at_the_root(void)
{
    op_t op = {.name = "room", .card = CARD_OLD_FILES};
    build_card(&op);
    const uint8_t *bs = ramdisk_sector(0);
    const uint32_t root = (bs[14] | bs[15] << 8) + bs[16] * le32(bs + 36);
    uint32_t offset = 0;
    while (offset < RAMDISK_SECTOR && memcmp(ramdisk_sector(root) + offset, "WATCHD~1   ", 11) != 0) {
        offset += 32;
    }
    TEST_ASSERT_TRUE(offset < RAMDISK_SECTOR);
    const uint32_t clusters[] = {0, le32(bs + 44)};
    for (int i = 0; i < 2; i++) {
        ramdisk_restore();
        const uint8_t hi[2] = {(uint8_t)(clusters[i] >> 16), (uint8_t)(clusters[i] >> 24)};
        const uint8_t lo[2] = {(uint8_t)clusters[i], (uint8_t)(clusters[i] >> 8)};
        poke(root, offset + 20, hi, 2);
        poke(root, offset + 26, lo, 2);
        mount();
        const uint8_t runs[] = {1};
        TEST_ASSERT_EQUAL_HEX(ESP_FAIL, cardfs_fat_room("watchdeck", runs, 1, s_drive));
        ramdisk_reset_count();
        TEST_ASSERT_EQUAL_HEX(ESP_FAIL, cardfs_write_new(s_image_name, s_image, 10));
        TEST_ASSERT_EQUAL(0, ramdisk_writes());
        unmount();
    }
}

static void make_data(void)
{
    // About 1.5 KB, so the new sleep.cfg spans three clusters where the old one had one.
    size_t n = snprintf(s_sleep_cfg, sizeof(s_sleep_cfg), "{\"t\":\"sleep\",\"text\":\"");
    while (n < sizeof(s_sleep_cfg) - 100) {
        n += snprintf(s_sleep_cfg + n, sizeof(s_sleep_cfg) - n, "Back soon %03u. ", (unsigned)n);
    }
    n += snprintf(s_sleep_cfg + n, sizeof(s_sleep_cfg) - n, "\",\"offline_after_s\":10}");
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
    snprintf(s_image_name, sizeof(s_image_name), "watchdeck/%02x%02x%02x%02x.img", sha[0], sha[1], sha[2], sha[3]);
}

void app_main(void)
{
    if (psa_crypto_init() != PSA_SUCCESS) {
        printf("psa_crypto_init failed\n");
        exit(1);
    }
    ramdisk_create(1, 0);
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
    esp_log_level_set("cardfs", ESP_LOG_NONE); // thousands of recoveries
    make_data();

    UNITY_BEGIN();
    RUN_TEST(test_fatwalk_finds_a_cross_link_a_free_cluster_stale_directory_bytes_and_an_orphan_long_name);
    RUN_TEST(test_room_refuses_a_directory_with_an_entry_past_its_end_marker);
    RUN_TEST(test_cuts_first_wifi_txt);
    RUN_TEST(test_cuts_first_wifi_txt_into_a_root_with_exactly_enough_room);
    RUN_TEST(test_cuts_first_wifi_txt_into_a_nearly_full_root_is_refused);
    RUN_TEST(test_cuts_wifi_txt_replace);
    RUN_TEST(test_cuts_wifi_txt_replace_after_a_free_space_count);
    RUN_TEST(test_cuts_wifi_txt_replace_with_a_full_watchdeck);
    RUN_TEST(test_cuts_sleep_cfg_replace);
    RUN_TEST(test_cuts_first_sleep_cfg_into_a_watchdeck_with_exactly_enough_room);
    RUN_TEST(test_cuts_first_sleep_cfg_into_a_nearly_full_watchdeck_is_refused);
    RUN_TEST(test_cuts_1_mib_image);
    RUN_TEST(test_cuts_1_mib_image_into_a_hole_in_a_full_watchdeck);
    RUN_TEST(test_cuts_1_mib_image_into_a_full_watchdeck_is_refused);
    RUN_TEST(test_cuts_a_long_name_within_a_sector);
    RUN_TEST(test_cuts_a_long_name_across_a_sector_is_refused);
    RUN_TEST(test_cuts_first_wifi_txt_with_watchdeck_across_a_sector_is_refused);
    RUN_TEST(test_cuts_remove_an_image);
    RUN_TEST(test_cuts_first_wifi_txt_with_4_kib_clusters);
    RUN_TEST(test_cuts_first_wifi_txt_into_a_root_with_exactly_enough_room_with_4_kib_clusters);
    RUN_TEST(test_cuts_first_wifi_txt_into_a_nearly_full_root_with_4_kib_clusters_is_refused);
    RUN_TEST(test_cuts_sleep_cfg_replace_with_4_kib_clusters);
    RUN_TEST(test_cuts_1_mib_image_with_4_kib_clusters);
    RUN_TEST(test_cuts_retry_after_a_write_error_never_leaves_a_torn_wifi_txt);
    RUN_TEST(test_cuts_one_failed_write_is_retried_from_the_record);
    RUN_TEST(test_room_refuses_a_watchdeck_that_starts_at_the_root);
    RUN_TEST(test_free_space_counts_the_clusters_and_writes_nothing);
    RUN_TEST(test_a_fat16_volume_is_refused);
    RUN_TEST(test_format_layout_by_card_size);
    RUN_TEST(test_format_128_gib_exfat_card);
    RUN_TEST(test_format_8_gib_blank_card);
    RUN_TEST(test_format_gpt_card);
    RUN_TEST(test_format_cut_at_every_write);
    // Returning from app_main does not end a linux-target process, so the failure count becomes the exit status.
    exit(UNITY_END());
}
