// cardfs's volume functions for FatFs, FAT32 only (the card is FAT32; FAT12/16 have a fixed root and other FAT
// entry sizes this code doesn't handle). The room function reads a directory's entries and FAT chain straight from
// the disk and replays dir_alloc (ff.c) for the entry runs a call will add: each run goes at the first run of free
// entries (first byte 0x00 or 0xE5) long enough, scanning from the start, and dir_alloc grows the directory only
// when it scans off the end of its last cluster. Only cardfs writes to the card and it holds its mutex here, and
// every FatFs call it makes syncs before it returns, so the disk holds what FatFs would read.
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

// The mounted FAT32 volume on drive ctx, and the start cluster of dir on it (0 for the root).
static esp_err_t open_volume(void *ctx, const char *dir, FATFS **fs, DWORD *start)
{
    char path[32];
    snprintf(path, sizeof(path), "%s/%s", (const char *)ctx, dir);
    FF_DIR dj = {0};
    const FRESULT res = f_opendir(&dj, path);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_opendir %s: FRESULT %d", path, res);
        return ESP_FAIL;
    }
    *fs = dj.obj.fs;
    *start = dj.obj.sclust;
    f_closedir(&dj);
    if ((*fs)->fs_type != FS_FAT32) {
        ESP_LOGE(TAG, "the card is FAT type %d, not FAT32", (*fs)->fs_type);
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static UINT sector_size(const FATFS *fs)
{
#if FF_MAX_SS != FF_MIN_SS
    return fs->ssize;
#else
    return FF_MAX_SS;
#endif
}

esp_err_t cardfs_fat_prepare(void *ctx)
{
    FATFS *fs;
    DWORD start;
    const esp_err_t err = open_volume(ctx, "", &fs, &start);
    if (err == ESP_OK) {
        fs->free_clst = fs->last_clst = 0xFFFFFFFF; // FSINFO's hints, which a cut can leave stale: count afresh
    }
    return err;
}

static bool read_sector(scan_t *s, LBA_t lba)
{
    if (s->sec_valid && s->sec_lba == lba) {
        return true;
    }
    s->sec_valid = ff_disk_read(s->fs->pdrv, s->sec, lba, 1) == RES_OK;
    s->sec_lba = lba;
    return s->sec_valid;
}

// The cluster after c in its chain, CHAIN_END at the end, or false on a read error.
static bool fat_next(scan_t *s, DWORD c, DWORD *next)
{
    if (!read_sector(s, s->fs->fatbase + c * 4 / s->ss)) {
        return false;
    }
    const uint8_t *b = s->sec + c * 4 % s->ss;
    *next = (b[0] | (DWORD)b[1] << 8 | (DWORD)b[2] << 16 | (DWORD)b[3] << 24) & 0x0FFFFFFF;
    *next = *next >= 0x0FFFFFF8 ? CHAIN_END : *next;
    return true;
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

// Every entry of the directory whose chain starts at cluster c into s->slot.
static esp_err_t scan_dir(scan_t *s, DWORD c)
{
    FATFS *fs = s->fs;
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
    const size_t per_sector = s->ss / ENTRY;
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
        const size_t first = i - len;
        // FatFs writes the sector with the long-name entries before the one with the 8.3 entry, so a cut between
        // them would leave orphan long-name entries.
        if (first / per_sector != (i - 1) / per_sector) {
            ESP_LOGW(TAG, "room: a new name's %u entries in /%s would cross a sector boundary at entry %u",
                     (unsigned)len, dir, (unsigned)first);
            return CARDFS_ERR_DIR_FULL;
        }
        memset(s->slot + first, SLOT_USED, len);
    }
    return ESP_OK;
}

esp_err_t cardfs_fat_room(const char *dir, const uint8_t *runs, size_t n, void *ctx)
{
    scan_t s = {0};
    DWORD start;
    esp_err_t err = open_volume(ctx, dir, &s.fs, &start);
    if (err != ESP_OK) {
        return err;
    }
    const DWORD root = (DWORD)s.fs->dirbase;
    if (dir[0] && (start == 0 || start == root)) {
        // A damaged entry: FatFs reads a directory with start cluster 0 as the root.
        ESP_LOGE(TAG, "room: /%s starts at the root's cluster; check the card on a PC", dir);
        return ESP_FAIL;
    }
    s.ss = sector_size(s.fs);
    s.sec = malloc(s.ss);
    err = s.sec ? scan_dir(&s, start ? start : root) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = replay(&s, dir, runs, n);
    } else if (err == ESP_FAIL) {
        ESP_LOGE(TAG, "room: reading /%s failed", dir);
    }
    free(s.slot);
    free(s.sec);
    return err;
}

esp_err_t cardfs_fat_space(uint64_t *total, uint64_t *free_bytes, void *ctx)
{
    FATFS *fs;
    DWORD free_clusters;
    const FRESULT res = f_getfree(ctx, &free_clusters, &fs);
    if (res != FR_OK) {
        ESP_LOGE(TAG, "f_getfree: FRESULT %d", res);
        return ESP_FAIL;
    }
    const uint64_t cluster = (uint64_t)fs->csize * sector_size(fs);
    *total = (uint64_t)(fs->n_fatent - 2) * cluster;
    *free_bytes = (uint64_t)free_clusters * cluster;
    return ESP_OK;
}
