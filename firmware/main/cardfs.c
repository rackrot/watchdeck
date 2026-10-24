#include "cardfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#define DIR_NAME "watchdeck"
#define ROOT_MAX 15
#define PATH_LEN (ROOT_MAX + sizeof("/" DIR_NAME "/") + CARDFS_NAME_MAX)
#define SHA_LEN 32
#define RUNS_MAX 3 // most new entries one call adds to one directory

static const char *TAG = "cardfs";

// The small files and their intent records. Recovery looks for exactly these records.
static const struct {
    const char *rel;
    const char *record;
} SMALL[] = {
    {"wifi.txt", DIR_NAME "/wifi.new"},
    {DIR_NAME "/sleep.cfg", DIR_NAME "/sleep.new"},
};

static char s_root[ROOT_MAX + 1];
static cardfs_volume_t s_vol;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_writable;
static bool s_tainted;         // a card access failed on this mount; see cardfs.h
static uint32_t s_generation;  // counts small writes since boot; recovery logs the one it redid
static int s_fail_sha256 = -1; // cardfs_test_fail_sha256

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void path_of(char out[PATH_LEN], const char *rel)
{
    snprintf(out, PATH_LEN, "%s/%s", s_root, rel);
}

static const char *small_record(const char *rel)
{
    for (size_t i = 0; i < sizeof(SMALL) / sizeof(SMALL[0]); i++) {
        if (strcmp(rel, SMALL[i].rel) == 0) {
            return SMALL[i].record;
        }
    }
    return NULL;
}

static void taint(const char *why)
{
    if (!s_tainted) {
        ESP_LOGE(TAG, "%s; no more writes until the card is mounted again", why);
    }
    s_tainted = true;
}

// Passes a failed call's errno through, tainting cardfs unless it only says a name is absent or present, or memory
// ran out.
static int io_err(int err)
{
    if (err != 0 && err != ENOENT && err != EEXIST && err != ENOMEM) {
        taint(strerror(err));
    }
    return err;
}

void cardfs_test_fail_sha256(int after)
{
    s_fail_sha256 = after;
}

static bool sha256(const void *data, size_t len, uint8_t out[SHA_LEN])
{
    if (s_fail_sha256 >= 0 && s_fail_sha256-- == 0) {
        ESP_LOGE(TAG, "psa_hash_compute: failed for the test");
        return false;
    }
    size_t out_len;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, data, len, out, SHA_LEN, &out_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute: %d", (int)status);
        return false;
    }
    return true;
}

static int write_all(int fd, const uint8_t *p, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, p, len < CARDFS_PIECE ? len : CARDFS_PIECE);
        if (n <= 0) {
            return io_err(n < 0 ? errno : EIO);
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

// Creates or truncates path, writes head then body, fsyncs. Returns 0 or an errno.
static int put_file(const char *path, const void *head, size_t head_len, const void *body, size_t body_len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return io_err(errno);
    }
    int err = write_all(fd, head, head_len);
    if (err == 0) {
        err = write_all(fd, body, body_len);
    }
    if (err == 0 && fsync(fd) != 0) {
        err = io_err(errno);
    }
    if (close(fd) != 0 && err == 0) {
        err = io_err(errno);
    }
    return err;
}

// Reads path into buf. Returns 0, EFBIG when the file holds more than cap bytes, or an errno (ENOENT: absent).
static int get_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return io_err(errno);
    }
    int err = 0;
    for (;;) {
        size_t want = cap - *len < CARDFS_PIECE ? cap - *len : CARDFS_PIECE;
        uint8_t extra;
        ssize_t n = want > 0 ? read(fd, buf + *len, want) : read(fd, &extra, 1);
        if (n < 0) {
            err = io_err(errno);
            break;
        }
        if (n == 0) {
            break;
        }
        if (want == 0) {
            err = EFBIG;
            break;
        }
        *len += (size_t)n;
    }
    close(fd);
    return err;
}

// Returns 0 when path holds exactly data, EILSEQ when it differs, or an errno (ENOENT: absent).
static int same_contents(const char *path, const void *data, size_t len)
{
    uint8_t *buf = malloc(len + 1);
    if (!buf) {
        return ENOMEM;
    }
    size_t got;
    int err = get_file(path, buf, len, &got);
    if (err == EFBIG || (err == 0 && (got != len || memcmp(buf, data, len) != 0))) {
        err = EILSEQ;
    }
    free(buf);
    return err;
}

// Writes data to path and reads it back. A file that reads back different taints cardfs: the card lost a write.
static int put_checked(const char *path, const void *data, size_t len)
{
    int err = put_file(path, NULL, 0, data, len);
    if (err == 0) {
        err = same_contents(path, data, len);
        if (err == EILSEQ || err == ENOENT) {
            taint("a file read back different from what was written");
        }
    }
    return err;
}

// Empties a record that exists; an absent one is left absent, since creating it here would skip the room check.
// IDF's VFS honours O_TRUNC only together with O_CREAT (FA_CREATE_ALWAYS, vfs_fat.c fat_mode_conv), hence the stat
// first. FA_CREATE_ALWAYS clears the entry before it frees the chain; truncate() and ftruncate() free the chain
// first, so they're never used here.
static int clear_record(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno == ENOENT ? 0 : io_err(errno);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return io_err(errno);
    }
    return close(fd) == 0 ? 0 : io_err(errno);
}

// Returns 0 when path can be found by name with len bytes. The room check keeps FatFs from placing an entry where
// no lookup finds it; this is the second line, turning such a file into an error before anything relies on it.
static int findable(const char *path, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        const int err = errno;
        taint("a file just written can't be found by name");
        return err;
    }
    if ((size_t)st.st_size != len) {
        taint("a file just written has the wrong size");
        return EILSEQ;
    }
    return 0;
}

// 1 when rel exists, 0 when it doesn't, or a negative errno.
static int exists(const char *rel)
{
    char path[PATH_LEN];
    path_of(path, rel);
    struct stat st;
    if (stat(path, &st) == 0) {
        return 1;
    }
    return errno == ENOENT ? 0 : -io_err(errno);
}

// Directory entries FatFs gives a name: one for a name it can store as 8.3 without a long name (one dot at most,
// a body of 1 to 8 and an extension of up to 3 characters, all one case; ours are lowercase), else one per 13
// characters of long name plus the 8.3 entry (ff.c create_name, dir_register).
static unsigned entries_for(const char *name)
{
    const size_t n = strlen(name);
    const char *dot = strchr(name, '.');
    const size_t body = dot ? (size_t)(dot - name) : n;
    const size_t ext = dot ? n - body - 1 : 0;
    if (body >= 1 && body <= 8 && ext <= 3 && (!dot || (ext >= 1 && !strchr(dot + 1, '.')))) {
        return 1;
    }
    return (unsigned)(n + 12) / 13 + 1;
}

// The new entries one call adds, in the order it adds them, per directory.
typedef struct {
    uint8_t root[RUNS_MAX];
    size_t n_root;
    uint8_t dir[RUNS_MAX];
    size_t n_dir;
    bool dir_new; // watchdeck/ is created, in a cluster f_mkdir zeroes first, so it has room
} plan_t;

static int plan_add(uint8_t *runs, size_t *n, const char *name)
{
    if (*n == RUNS_MAX) {
        return -EOVERFLOW;
    }
    runs[(*n)++] = (uint8_t)entries_for(name);
    return 0;
}

// Adds rel to the plan when it doesn't exist yet. Returns 0 or a negative errno.
static int plan_file(plan_t *p, const char *rel)
{
    const int e = exists(rel);
    if (e != 0) {
        return e < 0 ? e : 0;
    }
    const char *slash = strchr(rel, '/');
    if (!slash) {
        return plan_add(p->root, &p->n_root, rel);
    }
    return p->dir_new ? 0 : plan_add(p->dir, &p->n_dir, slash + 1);
}

// Adds watchdeck/ to the plan when it doesn't exist yet; called before plan_file for anything in it.
static int plan_dir(plan_t *p)
{
    const int e = exists(DIR_NAME);
    if (e != 0) {
        return e < 0 ? e : 0;
    }
    p->dir_new = true;
    return plan_add(p->root, &p->n_root, DIR_NAME);
}

// ESP_OK when every planned entry fits without FatFs growing its directory, CARDFS_ERR_DIR_FULL when one doesn't,
// ESP_ERR_INVALID_STATE when tainted, ESP_FAIL on a read error.
static esp_err_t check_room(const plan_t *p, int plan_err, const char *what)
{
    if (plan_err != 0) {
        ESP_LOGE(TAG, "%s: plan: %s", what, strerror(-plan_err));
        return ESP_FAIL;
    }
    if (s_tainted && (p->n_root > 0 || p->n_dir > 0)) {
        ESP_LOGE(TAG, "%s: no room check on a tainted mount", what);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (p->n_root > 0) {
        err = s_vol.room("", p->root, p->n_root, s_vol.ctx);
    }
    if (err == ESP_OK && p->n_dir > 0) {
        err = s_vol.room(DIR_NAME, p->dir, p->n_dir, s_vol.ctx);
    }
    if (err == CARDFS_ERR_DIR_FULL) {
        ESP_LOGE(TAG,
                 "%s: refused, a directory on the card has no room for it; delete or add a few files there on a PC",
                 what);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: room check: %s", what, esp_err_to_name(err));
        taint("the room check failed");
        err = ESP_FAIL;
    }
    return err;
}

static int ensure_dir(void)
{
    char path[PATH_LEN];
    path_of(path, DIR_NAME);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return io_err(errno);
    }
    return 0;
}

static bool lock(void)
{
    if (!s_lock) {
        ESP_LOGE(TAG, "used before cardfs_init");
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    return true;
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

// ESP_OK when a write may start: writable and not tainted.
static esp_err_t may_write(void)
{
    if (!s_writable) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_tainted) {
        ESP_LOGE(TAG, "refused: an earlier card access failed on this mount");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t prepare(void)
{
    const esp_err_t err = s_vol.prepare ? s_vol.prepare(s_vol.ctx) : ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the card can't be written: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t cardfs_init(const char *root, const cardfs_volume_t *vol)
{
    if (strlen(root) > ROOT_MAX || !vol || !vol->room) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    lock();
    strcpy(s_root, root);
    s_vol = *vol;
    s_writable = false;
    s_tainted = false;
    const esp_err_t err = prepare();
    unlock();
    return err;
}

esp_err_t cardfs_set_writable(bool writable)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (!writable) {
        s_tainted = false; // the caller remounts before it turns writes back on
        s_writable = false;
    } else {
        err = prepare();
        s_writable = err == ESP_OK;
    }
    unlock();
    return err;
}

bool cardfs_writable(void)
{
    return s_writable;
}

bool cardfs_name_ok(const char *rel)
{
    if (strcmp(rel, "wifi.txt") == 0) {
        return true;
    }
    static const char prefix[] = DIR_NAME "/";
    if (strncmp(rel, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *name = rel + sizeof(prefix) - 1;
    const size_t n = strlen(name);
    // FatFs drops a trailing '.', so "x." would name "x".
    if (n == 0 || n > CARDFS_NAME_MAX || name[0] == '.' || name[n - 1] == '.' || strstr(name, "..")) {
        return false;
    }
    if (n >= 4 && strcmp(name + n - 4, ".new") == 0) {
        return false; // an intent record
    }
    for (size_t i = 0; i < n; i++) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

// Checks the record in buf (n bytes). ESP_OK with *problem NULL: valid. ESP_OK with *problem set: torn or
// damaged, so it can be cleared. ESP_FAIL: it couldn't be checked, so it must stay.
static esp_err_t check_record(const uint8_t *buf, size_t n, const char **problem)
{
    *problem = NULL;
    if (n < CARDFS_RECORD_HDR) {
        *problem = "short header";
    } else if (memcmp(buf, "WDI1", 4) != 0) {
        *problem = "bad magic";
    } else if (get_le32(buf + 44) != esp_rom_crc32_le(0, buf, 44)) {
        *problem = "bad header CRC";
    } else if (get_le32(buf + 4) > CARDFS_SMALL_MAX || n != CARDFS_RECORD_HDR + get_le32(buf + 4)) {
        *problem = "payload length differs";
    } else {
        uint8_t hash[SHA_LEN];
        if (!sha256(buf + CARDFS_RECORD_HDR, get_le32(buf + 4), hash)) {
            return ESP_FAIL;
        }
        if (memcmp(hash, buf + 12, SHA_LEN) != 0) {
            *problem = "bad SHA-256";
        }
    }
    return ESP_OK;
}

// Resolves rel's record: ESP_OK when none is pending any more; otherwise it's left in place for the next try, and
// the error is ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE (tainted) or CARDFS_ERR_DIR_FULL.
static esp_err_t recover_one(const char *rel, const char *record)
{
    char rec_path[PATH_LEN];
    char rel_path[PATH_LEN];
    path_of(rec_path, record);
    path_of(rel_path, rel);

    uint8_t *buf = malloc(CARDFS_RECORD_HDR + CARDFS_SMALL_MAX);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    const char *problem = NULL;
    size_t n;
    int err = get_file(rec_path, buf, CARDFS_RECORD_HDR + CARDFS_SMALL_MAX, &n);
    if (err == ENOENT || (err == 0 && n == 0)) {
        goto out; // no record, or an empty one: no write in progress
    }
    if (err == EFBIG) {
        problem = "too long";
    } else if (err != 0) {
        ESP_LOGE(TAG, "read %s: %s; left for the next try", record, strerror(err));
        ret = ESP_FAIL;
        goto out;
    } else if (check_record(buf, n, &problem) != ESP_OK) {
        ESP_LOGE(TAG, "%s couldn't be checked; left for the next try", record);
        ret = ESP_FAIL;
        goto out;
    }
    if (problem) {
        // Cut while the record was written, so rel was never touched.
        ESP_LOGW(TAG, "%s: %s, cleared; %s untouched", record, problem, rel);
    } else {
        const uint32_t len = get_le32(buf + 4);
        const uint8_t *payload = buf + CARDFS_RECORD_HDR;
        if (same_contents(rel_path, payload, len) == 0) {
            ESP_LOGI(TAG, "%s: write %lu was complete", rel, (unsigned long)get_le32(buf + 8));
        } else {
            plan_t plan = {0};
            ret = check_room(&plan, plan_file(&plan, rel), rel);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "%s left for the next try", record);
                goto out;
            }
            err = put_checked(rel_path, payload, len);
            if (err != 0) {
                ESP_LOGE(TAG, "redo %s: %s; %s left for the next try", rel, strerror(err), record);
                ret = ESP_FAIL;
                goto out;
            }
            ESP_LOGW(TAG, "%s: redid write %lu (%lu bytes)", rel, (unsigned long)get_le32(buf + 8), (unsigned long)len);
        }
    }
    err = clear_record(rec_path);
    if (err != 0) {
        ESP_LOGE(TAG, "clear %s: %s", record, strerror(err));
        ret = ESP_FAIL;
    }
out:
    free(buf);
    return ret;
}

static esp_err_t write_small_locked(const char *rel, const char *record, const uint8_t *data, size_t len)
{
    esp_err_t ret = may_write();
    if (ret != ESP_OK) {
        return ret;
    }
    // A record an earlier write left (an I/O error after it was written) must be applied before this write
    // empties it, or a cut in this write's step 1 would leave rel as that write tore it.
    ret = recover_one(rel, record);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "%s: an earlier write is still pending, so this one is refused", rel);
        return ret;
    }
    plan_t plan = {0};
    int plan_err = plan_dir(&plan);
    if (plan_err == 0) {
        plan_err = plan_file(&plan, record);
    }
    if (plan_err == 0) {
        plan_err = plan_file(&plan, rel);
    }
    ret = check_room(&plan, plan_err, rel);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t hdr[CARDFS_RECORD_HDR];
    memcpy(hdr, "WDI1", 4);
    put_le32(hdr + 4, (uint32_t)len);
    put_le32(hdr + 8, ++s_generation);
    if (!sha256(data, len, hdr + 12)) {
        return ESP_FAIL;
    }
    put_le32(hdr + 44, esp_rom_crc32_le(0, hdr, 44));

    char rec_path[PATH_LEN];
    char rel_path[PATH_LEN];
    path_of(rec_path, record);
    path_of(rel_path, rel);

    int err = ensure_dir();
    if (err == 0) {
        err = put_file(rec_path, hdr, sizeof(hdr), data, len);
        if (err != 0) {
            clear_record(rec_path); // torn; recovery would clear it anyway
        }
    }
    if (err == 0) {
        err = findable(rec_path, sizeof(hdr) + len);
    }
    if (err != 0) {
        ESP_LOGE(TAG, "write %s: %s; %s untouched", record, strerror(err), rel);
        return ESP_FAIL;
    }
    err = put_checked(rel_path, data, len);
    if (err != 0) {
        ESP_LOGE(TAG, "write %s: %s; trying %s once more", rel, strerror(err), record);
        if (recover_one(rel, record) == ESP_OK && same_contents(rel_path, data, len) == 0) {
            ESP_LOGW(TAG, "wrote %s (%u bytes) at the second try", rel, (unsigned)len);
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    err = clear_record(rec_path);
    if (err != 0) {
        ESP_LOGW(TAG, "wrote %s, but clear %s: %s", rel, record, strerror(err));
    }
    ESP_LOGI(TAG, "wrote %s (%u bytes)", rel, (unsigned)len);
    return ESP_OK;
}

esp_err_t cardfs_write_small(const char *rel, const void *data, size_t len)
{
    const char *record = small_record(rel);
    if (!record) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > CARDFS_SMALL_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = write_small_locked(rel, record, data, len);
    unlock();
    return err;
}

static esp_err_t write_new_locked(const char *rel, const uint8_t *data, size_t len)
{
    esp_err_t ret = may_write();
    if (ret != ESP_OK) {
        return ret;
    }
    plan_t plan = {0};
    int plan_err = plan_dir(&plan);
    if (plan_err == 0) {
        const int e = exists(rel);
        if (e == 1) {
            return ESP_ERR_NOT_ALLOWED;
        }
        plan_err = e < 0 ? e : plan_file(&plan, rel);
    }
    ret = check_room(&plan, plan_err, rel);
    if (ret != ESP_OK) {
        return ret;
    }
    char path[PATH_LEN];
    path_of(path, rel);
    int err = ensure_dir();
    if (err != 0) {
        ESP_LOGE(TAG, "mkdir %s: %s", DIR_NAME, strerror(err));
        return ESP_FAIL;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            return ESP_ERR_NOT_ALLOWED;
        }
        ESP_LOGE(TAG, "create %s: %s", rel, strerror(io_err(errno)));
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        close(fd); // created where no lookup finds it, so write nothing into it
        taint("a file just created can't be found by name");
        ESP_LOGE(TAG, "create %s: not found by name after creating it; check the card", rel);
        return ESP_FAIL;
    }
    err = write_all(fd, data, len);
    if (err == 0 && fsync(fd) != 0) {
        err = io_err(errno);
    }
    if (close(fd) != 0 && err == 0) {
        err = io_err(errno);
    }
    if (err == 0) {
        err = findable(path, len);
    }
    if (err != 0) {
        ESP_LOGE(TAG, "write %s: %s", rel, strerror(err));
        unlink(path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "wrote %s (%u bytes)", rel, (unsigned)len);
    return ESP_OK;
}

esp_err_t cardfs_write_new(const char *rel, const void *data, size_t len)
{
    if (!cardfs_name_ok(rel) || small_record(rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = write_new_locked(rel, data, len);
    unlock();
    return err;
}

esp_err_t cardfs_read(const char *rel, void *buf, size_t cap, size_t *len)
{
    if (!cardfs_name_ok(rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[PATH_LEN];
    path_of(path, rel);
    int err = get_file(path, buf, cap, len);
    unlock();
    if (err == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == EFBIG) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (err != 0) {
        ESP_LOGE(TAG, "read %s: %s", rel, strerror(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Hashes path, which stat says is len bytes. Returns 0, EILSEQ on a short read, or an errno.
static int hash_file(const char *path, size_t len, uint8_t out[SHA_LEN])
{
    uint8_t *buf = malloc(CARDFS_PIECE);
    if (!buf) {
        return ENOMEM;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return io_err(errno);
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    int err = 0;
    size_t total = 0;
    while (status == PSA_SUCCESS) {
        ssize_t n = read(fd, buf, CARDFS_PIECE);
        if (n < 0) {
            err = io_err(errno);
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
        status = psa_hash_update(&op, buf, (size_t)n);
    }
    close(fd);
    free(buf);
    size_t out_len;
    if (err == 0 && status == PSA_SUCCESS) {
        status = psa_hash_finish(&op, out, SHA_LEN, &out_len);
    }
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa hash: %d", (int)status);
        err = EIO;
    }
    psa_hash_abort(&op);
    if (err == 0 && total != len) {
        err = EILSEQ;
    }
    return err;
}

esp_err_t cardfs_verify(const char *rel, size_t len, const uint8_t *digest, size_t digest_len)
{
    if (!cardfs_name_ok(rel) || digest_len == 0 || digest_len > SHA_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[PATH_LEN];
    path_of(path, rel);
    esp_err_t ret;
    struct stat st;
    uint8_t hash[SHA_LEN];
    if (stat(path, &st) != 0) {
        ret = io_err(errno) == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else if (!S_ISREG(st.st_mode) || (size_t)st.st_size != len) {
        ret = ESP_ERR_INVALID_CRC;
    } else {
        int err = hash_file(path, len, hash);
        if (err == EILSEQ) {
            ret = ESP_ERR_INVALID_CRC;
        } else if (err != 0) {
            ESP_LOGE(TAG, "verify %s: %s", rel, strerror(err));
            ret = ESP_FAIL;
        } else {
            ret = memcmp(hash, digest, digest_len) == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
        }
    }
    unlock();
    return ret;
}

esp_err_t cardfs_remove(const char *rel)
{
    if (!cardfs_name_ok(rel)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[PATH_LEN];
    path_of(path, rel);
    esp_err_t ret = may_write();
    if (ret == ESP_OK && unlink(path) != 0) {
        if (errno == ENOENT) {
            ret = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGE(TAG, "delete %s: %s", rel, strerror(io_err(errno)));
            ret = ESP_FAIL;
        }
    }
    unlock();
    return ret;
}

typedef struct {
    char *name;
    size_t size;
    bool is_dir;
} list_entry_t;

typedef struct {
    list_entry_t *e;
    size_t n, cap;
} list_t;

static void list_free(list_t *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->e[i].name);
    }
    free(l->e);
}

static esp_err_t list_add(list_t *l, const char *name, const struct stat *st)
{
    if (l->n == l->cap) {
        const size_t cap = l->cap ? 2 * l->cap : 16;
        list_entry_t *e = realloc(l->e, cap * sizeof(*e));
        if (!e) {
            return ESP_ERR_NO_MEM;
        }
        l->e = e;
        l->cap = cap;
    }
    char *copy = strdup(name);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    l->e[l->n++] = (list_entry_t){.name = copy, .size = (size_t)st->st_size, .is_dir = S_ISDIR(st->st_mode)};
    return ESP_OK;
}

static esp_err_t list_locked(list_t *l)
{
    char path[PATH_LEN + 256];
    snprintf(path, sizeof(path), "%s/%s", s_root, DIR_NAME);
    DIR *dir = opendir(path);
    if (!dir) {
        return io_err(errno) == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    esp_err_t ret = ESP_OK;
    for (;;) {
        errno = 0;
        const struct dirent *ent = readdir(dir);
        if (!ent) {
            if (errno != 0) {
                ESP_LOGE(TAG, "list %s: %s", DIR_NAME, strerror(io_err(errno)));
                ret = ESP_FAIL;
            }
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s/%s", s_root, DIR_NAME, ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            ESP_LOGE(TAG, "stat %s/%s: %s", DIR_NAME, ent->d_name, strerror(io_err(errno)));
            ret = ESP_FAIL;
            break;
        }
        ret = list_add(l, ent->d_name, &st);
        if (ret != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    return ret;
}

esp_err_t cardfs_list(cardfs_list_cb_t cb, void *ctx)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    list_t l = {0};
    const esp_err_t ret = list_locked(&l);
    unlock();
    for (size_t i = 0; ret == ESP_OK && i < l.n; i++) {
        cb(l.e[i].name, l.e[i].size, l.e[i].is_dir, ctx);
    }
    list_free(&l);
    return ret;
}

esp_err_t cardfs_recover(void)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = may_write();
    for (size_t i = 0; ret != ESP_ERR_INVALID_STATE && i < sizeof(SMALL) / sizeof(SMALL[0]); i++) {
        if (s_tainted) {
            ret = ESP_FAIL; // the rest wait for the next mount
            break;
        }
        if (recover_one(SMALL[i].rel, SMALL[i].record) != ESP_OK) {
            ret = ESP_FAIL;
        }
    }
    unlock();
    return ret;
}
