// cardfs's room function for FatFs. It reads a directory's entries and FAT chain straight from the disk and replays
// dir_alloc (ff.c) for the entry runs a call will add: each run goes at the first run of free entries (first byte
// 0x00 or 0xE5) long enough, scanning from the start, and dir_alloc grows the directory only when it scans off the
// end of its last cluster. Only cardfs writes to the card and it holds its mutex here, and every FatFs call it makes
// syncs before it returns, so the disk holds what FatFs would read.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cardfs.h"
#include "diskio_impl.h"
#include "esp_log.h"

#define ENTRY 32
#define DIR_BYTES_MAX 0x200000 // FatFs's MAX_DIR: it never reads or grows a directory past this
#define CHAIN_END 0xFFFFFFFFu

enum { SLOT_END, SLOT_DELETED, SLOT_USED }; // first byte 0x00, 0xE5, anything else

static const char *TAG = "cardfs";

typedef struct {
    FATFS *fs;
    UINT ss;
    uint8_t *sec;
    LBA_t sec_lba;
    bool sec_valid;
    uint8_t *slot;
    size_t n_slots, cap;
} scan_t;

static bool read_sector(scan_t *s, LBA_t lba)
{
    if (s->sec_valid && s->sec_lba == lba) {
        return true;
    }
    s->sec_valid = ff_disk_read(s->fs->pdrv, s->sec, lba, 1) == RES_OK;
    s->sec_lba = lba;
    return s->sec_valid;
}

static bool fat_bytes(scan_t *s, DWORD off, uint8_t *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (!read_sector(s, s->fs->fatbase + (off + i) / s->ss)) {
            return false;
        }
        b[i] = s->sec[(off + i) % s->ss];
    }
    return true;
}

// The cluster after c in its chain, CHAIN_END at the end, or false on a read error.
static bool fat_next(scan_t *s, DWORD c, DWORD *next)
{
    uint8_t b[4];
    switch (s->fs->fs_type) {
    case FS_FAT12:
        if (!fat_bytes(s, c + c / 2, b, 2)) {
            return false;
        }
        *next = c & 1 ? (b[0] | (DWORD)b[1] << 8) >> 4 : (b[0] | (DWORD)b[1] << 8) & 0xFFF;
        *next = *next >= 0xFF8 ? CHAIN_END : *next;
        return true;
    case FS_FAT16:
        if (!fat_bytes(s, c * 2, b, 2)) {
            return false;
        }
        *next = b[0] | (DWORD)b[1] << 8;
        *next = *next >= 0xFFF8 ? CHAIN_END : *next;
        return true;
    default:
        if (!fat_bytes(s, c * 4, b, 4)) {
            return false;
        }
        *next = (b[0] | (DWORD)b[1] << 8 | (DWORD)b[2] << 16 | (DWORD)b[3] << 24) & 0x0FFFFFFF;
        *next = *next >= 0x0FFFFFF8 ? CHAIN_END : *next;
        return true;
    }
}

static esp_err_t add_sector(scan_t *s, LBA_t lba)
{
    if (!read_sector(s, lba)) {
        return ESP_FAIL;
    }
    const size_t n = s->ss / ENTRY;
    if (s->n_slots + n > s->cap) {
        const size_t cap = s->cap * 2 > s->n_slots + n ? s->cap * 2 : s->n_slots + n;
        uint8_t *slot = realloc(s->slot, cap);
        if (!slot) {
            return ESP_ERR_NO_MEM;
        }
        s->slot = slot;
        s->cap = cap;
    }
    for (size_t i = 0; i < n; i++) {
        const uint8_t first = s->sec[i * ENTRY];
        s->slot[s->n_slots++] = first == 0x00 ? SLOT_END : first == 0xE5 ? SLOT_DELETED : SLOT_USED;
    }
    return ESP_OK;
}

// Every entry of the directory whose table starts at cluster c (0: the root) into s->slot.
static esp_err_t scan_dir(scan_t *s, DWORD c)
{
    FATFS *fs = s->fs;
    if (c == 0 && fs->fs_type != FS_FAT32) { // the FAT12/16 root: a fixed table, never grown
        esp_err_t err = ESP_OK;
        for (UINT i = 0; err == ESP_OK && i < (UINT)fs->n_rootdir * ENTRY / s->ss; i++) {
            err = add_sector(s, fs->dirbase + i);
        }
        return err;
    }
    if (c == 0) {
        c = (DWORD)fs->dirbase;
    }
    const size_t clusters_max = DIR_BYTES_MAX / ((size_t)fs->csize * s->ss);
    for (size_t k = 0;; k++) {
        if (c < 2 || c >= fs->n_fatent || k == clusters_max) {
            ESP_LOGE(TAG, "room: broken directory chain at cluster %lu", (unsigned long)c);
            return ESP_FAIL;
        }
        for (UINT i = 0; i < fs->csize; i++) {
            const esp_err_t err = add_sector(s, fs->database + (LBA_t)fs->csize * (c - 2) + i);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (!fat_next(s, c, &c)) {
            return ESP_FAIL;
        }
        if (c == CHAIN_END) {
            return ESP_OK;
        }
    }
}

// dir_alloc for each run in turn, over the entries scan_dir read.
static esp_err_t replay(scan_t *s, const char *dir, const uint8_t *runs, size_t n)
{
    size_t marker = 0;
    while (marker < s->n_slots && s->slot[marker] != SLOT_END) {
        marker++;
    }
    for (size_t i = marker; i < s->n_slots; i++) {
        if (s->slot[i] == SLOT_USED) {
            ESP_LOGE(TAG, "room: /%s has an entry past its end marker (a cut while it grew?); check the card on a PC",
                     dir);
            return CARDFS_ERR_DIR_FULL;
        }
    }
    for (size_t r = 0; r < n; r++) {
        size_t len = 0;
        size_t i = 0;
        for (; i < s->n_slots && len < runs[r]; i++) {
            len = s->slot[i] == SLOT_USED ? 0 : len + 1;
        }
        if (len < runs[r]) {
            ESP_LOGW(TAG, "room: /%s has no %u free entries in a row in its %u, so FatFs would grow it", dir,
                     (unsigned)runs[r], (unsigned)s->n_slots);
            return CARDFS_ERR_DIR_FULL;
        }
        memset(s->slot + i - len, SLOT_USED, len);
    }
    return ESP_OK;
}

esp_err_t cardfs_fat_room(const char *dir, const uint8_t *runs, size_t n, void *ctx)
{
    char path[32];
    snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, dir);
    FF_DIR dj = {0};
    const FRESULT res = f_opendir(&dj, path);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "room: f_opendir %s: FRESULT %d", path, res);
        return ESP_FAIL;
    }
    scan_t s = {.fs = dj.obj.fs};
    const DWORD start = dj.obj.sclust;
    f_closedir(&dj);
#if FF_MAX_SS != FF_MIN_SS
    s.ss = s.fs->ssize;
#else
    s.ss = FF_MAX_SS;
#endif
    s.sec = malloc(s.ss);
    esp_err_t err = s.sec ? scan_dir(&s, start) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = replay(&s, dir, runs, n);
    } else if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "room: reading /%s failed", dir);
    }
    free(s.slot);
    free(s.sec);
    return err;
}
