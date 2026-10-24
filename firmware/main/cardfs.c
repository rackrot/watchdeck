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

static const char *TAG = "cardfs";

// The small files and their intent records. Recovery looks for exactly these records.
static const struct {
    const char *rel;
    const char *record;
} SMALL[] = {
    {"wifi.txt", DIR_NAME "/wifi.new"},
    {DIR_NAME "/sleep.json", DIR_NAME "/sleep.new"},
};

static char s_root[ROOT_MAX + 1];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static bool s_writable;
static uint32_t s_generation; // counts small writes since boot; recovery logs the one it redid

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

static bool sha256(const void *data, size_t len, uint8_t out[SHA_LEN])
{
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
            return n < 0 ? errno : EIO;
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
        return errno;
    }
    int err = write_all(fd, head, head_len);
    if (err == 0) {
        err = write_all(fd, body, body_len);
    }
    if (err == 0 && fsync(fd) != 0) {
        err = errno;
    }
    if (close(fd) != 0 && err == 0) {
        err = errno;
    }
    return err;
}

// Reads path into buf. Returns 0, EFBIG when the file holds more than cap bytes, or an errno (ENOENT: absent).
static int get_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    *len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno;
    }
    int err = 0;
    for (;;) {
        size_t want = cap - *len < CARDFS_PIECE ? cap - *len : CARDFS_PIECE;
        uint8_t extra;
        ssize_t n = want > 0 ? read(fd, buf + *len, want) : read(fd, &extra, 1);
        if (n < 0) {
            err = errno;
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

// Empties a record. It stays as a 0-byte file, so its directory entry is reused by the next write: FatFs links a
// directory's new cluster before zeroing it (ff.c dir_next, then dir_clear), so a cut while it grows a directory
// can leave stale bytes in it. O_TRUNC through FatFs clears the entry before it frees the chain; truncate() and
// ftruncate() free the chain first, so they're never used here.
static int clear_record(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return errno;
    }
    return close(fd) == 0 ? 0 : errno;
}

// Returns 0 when path can be found by name with len bytes. FatFs can place a new entry past the end marker of a
// directory with stale bytes in it, where no lookup finds it; this turns that into an error before anything relies
// on the file.
static int findable(const char *path, size_t len)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return errno;
    }
    return (size_t)st.st_size == len ? 0 : EILSEQ;
}

static int ensure_dir(void)
{
    char path[PATH_LEN];
    path_of(path, DIR_NAME);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return errno;
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

esp_err_t cardfs_init(const char *root)
{
    if (strlen(root) > ROOT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    lock();
    strcpy(s_root, root);
    s_writable = false;
    unlock();
    return ESP_OK;
}

void cardfs_set_writable(bool writable)
{
    if (lock()) {
        s_writable = writable;
        unlock();
    }
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

static esp_err_t write_small_locked(const char *rel, const char *record, const uint8_t *data, size_t len)
{
    if (!s_writable) {
        return ESP_ERR_INVALID_STATE;
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
    err = put_file(rel_path, NULL, 0, data, len);
    if (err == 0) {
        err = same_contents(rel_path, data, len);
    }
    if (err != 0) {
        ESP_LOGE(TAG, "write %s: %s; %s stays for recovery", rel, strerror(err), record);
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
    if (!s_writable) {
        return ESP_ERR_INVALID_STATE;
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
        ESP_LOGE(TAG, "create %s: %s", rel, strerror(errno));
        return ESP_FAIL;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        close(fd); // created where no lookup finds it, so write nothing into it
        ESP_LOGE(TAG, "create %s: not found by name after creating it; check the card", rel);
        return ESP_FAIL;
    }
    err = write_all(fd, data, len);
    if (err == 0 && fsync(fd) != 0) {
        err = errno;
    }
    if (close(fd) != 0 && err == 0) {
        err = errno;
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
        return errno;
    }
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    int err = 0;
    size_t total = 0;
    while (status == PSA_SUCCESS) {
        ssize_t n = read(fd, buf, CARDFS_PIECE);
        if (n < 0) {
            err = errno;
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
        ret = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
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
    esp_err_t ret = ESP_OK;
    char path[PATH_LEN];
    path_of(path, rel);
    if (!s_writable) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (unlink(path) != 0) {
        if (errno == ENOENT) {
            ret = ESP_ERR_NOT_FOUND;
        } else {
            ESP_LOGE(TAG, "delete %s: %s", rel, strerror(errno));
            ret = ESP_FAIL;
        }
    }
    unlock();
    return ret;
}

esp_err_t cardfs_list(cardfs_list_cb_t cb, void *ctx)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    char path[PATH_LEN + 256];
    snprintf(path, sizeof(path), "%s/%s", s_root, DIR_NAME);
    DIR *dir = opendir(path);
    esp_err_t ret = ESP_OK;
    if (!dir) {
        ret = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    } else {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
                continue;
            }
            snprintf(path, sizeof(path), "%s/%s/%s", s_root, DIR_NAME, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) {
                ESP_LOGW(TAG, "stat %s/%s: %s", DIR_NAME, ent->d_name, strerror(errno));
                continue;
            }
            cb(ent->d_name, (size_t)st.st_size, S_ISDIR(st.st_mode), ctx);
        }
        closedir(dir);
    }
    unlock();
    return ret;
}

// Why the record in buf (n bytes) can't be trusted, or NULL when it can.
static const char *record_problem(const uint8_t *buf, size_t n)
{
    if (n < CARDFS_RECORD_HDR) {
        return "short header";
    }
    if (memcmp(buf, "WDI1", 4) != 0) {
        return "bad magic";
    }
    if (get_le32(buf + 44) != esp_rom_crc32_le(0, buf, 44)) {
        return "bad header CRC";
    }
    const uint32_t len = get_le32(buf + 4);
    if (len > CARDFS_SMALL_MAX || n != CARDFS_RECORD_HDR + len) {
        return "payload length differs";
    }
    uint8_t hash[SHA_LEN];
    if (!sha256(buf + CARDFS_RECORD_HDR, len, hash)) {
        return "SHA-256 failed";
    }
    if (memcmp(hash, buf + 12, SHA_LEN) != 0) {
        return "bad SHA-256";
    }
    return NULL;
}

static esp_err_t recover_one(const char *rel, const char *record, uint8_t *buf)
{
    char rec_path[PATH_LEN];
    char rel_path[PATH_LEN];
    path_of(rec_path, record);
    path_of(rel_path, rel);

    size_t n;
    int err = get_file(rec_path, buf, CARDFS_RECORD_HDR + CARDFS_SMALL_MAX, &n);
    if (err == ENOENT || (err == 0 && n == 0)) {
        return ESP_OK; // no record, or an empty one: no write in progress
    }
    if (err != 0 && err != EFBIG) {
        ESP_LOGE(TAG, "read %s: %s; left for the next mount", record, strerror(err));
        return ESP_FAIL;
    }
    const char *problem = err == EFBIG ? "too long" : record_problem(buf, n);
    if (problem) {
        // Cut while the record was written, so rel was never touched.
        ESP_LOGW(TAG, "%s: %s, cleared; %s untouched", record, problem, rel);
    } else {
        const uint32_t len = get_le32(buf + 4);
        const uint8_t *payload = buf + CARDFS_RECORD_HDR;
        if (same_contents(rel_path, payload, len) == 0) {
            ESP_LOGI(TAG, "%s: write %lu was complete", rel, (unsigned long)get_le32(buf + 8));
        } else {
            err = put_file(rel_path, NULL, 0, payload, len);
            if (err == 0) {
                err = same_contents(rel_path, payload, len);
            }
            if (err != 0) {
                ESP_LOGE(TAG, "redo %s: %s; %s left for the next mount", rel, strerror(err), record);
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "%s: redid write %lu (%lu bytes)", rel, (unsigned long)get_le32(buf + 8), (unsigned long)len);
        }
    }
    err = clear_record(rec_path);
    if (err != 0) {
        ESP_LOGE(TAG, "clear %s: %s", record, strerror(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t cardfs_recover(void)
{
    if (!lock()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;
    uint8_t *buf = NULL;
    if (!s_writable) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (!(buf = malloc(CARDFS_RECORD_HDR + CARDFS_SMALL_MAX))) {
        ret = ESP_ERR_NO_MEM;
    } else {
        for (size_t i = 0; i < sizeof(SMALL) / sizeof(SMALL[0]); i++) {
            if (recover_one(SMALL[i].rel, SMALL[i].record, buf) != ESP_OK) {
                ret = ESP_FAIL;
            }
        }
    }
    free(buf);
    unlock();
    return ret;
}
