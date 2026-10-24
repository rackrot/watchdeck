#include "ramdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t **s_base;  // the snapshot's sectors; NULL: the seed's pattern, made on first read
static uint8_t **s_cur;   // the sectors written since the snapshot; NULL: as in the snapshot
static uint32_t *s_dirty; // the sectors with s_cur set
static uint32_t s_n_dirty;
static uint32_t s_sectors;
static uint32_t s_seed;
static uint32_t s_writes;
static enum { ARM_NONE, ARM_CUT, ARM_FAIL } s_arm;
static uint32_t s_left;
static ramdisk_tear_t s_tear;
static bool s_off;
static bool s_failing;

static void *xmalloc(size_t n)
{
    void *p = malloc(n);
    if (!p) {
        printf("ramdisk: out of memory\n");
        exit(1);
    }
    return p;
}

void ramdisk_create(uint32_t sectors, uint32_t seed)
{
    for (uint32_t i = 0; i < s_sectors; i++) {
        free(s_base[i]);
        free(s_cur[i]);
    }
    free(s_base);
    free(s_cur);
    free(s_dirty);
    s_sectors = sectors;
    s_seed = seed;
    s_base = calloc(sectors, sizeof(*s_base));
    s_cur = calloc(sectors, sizeof(*s_cur));
    s_dirty = calloc(sectors, sizeof(*s_dirty));
    if (!s_base || !s_cur || !s_dirty) {
        printf("ramdisk: no memory for %lu sectors\n", (unsigned long)sectors);
        exit(1);
    }
    s_n_dirty = 0;
    ramdisk_power_on();
    s_writes = 0;
}

uint32_t ramdisk_sectors(void)
{
    return s_sectors;
}

const uint8_t *ramdisk_sector(uint32_t lba)
{
    if (lba >= s_sectors) {
        printf("ramdisk: sector %lu past the end\n", (unsigned long)lba);
        exit(1);
    }
    if (s_cur[lba]) {
        return s_cur[lba];
    }
    if (!s_base[lba]) {
        uint8_t *p = xmalloc(RAMDISK_SECTOR);
        uint32_t x = (s_seed ^ (lba * 2654435761u)) | 1;
        for (int i = 0; i < RAMDISK_SECTOR; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            p[i] = (uint8_t)x;
        }
        s_base[lba] = p;
    }
    return s_base[lba];
}

void ramdisk_snapshot(void)
{
    for (uint32_t k = 0; k < s_n_dirty; k++) {
        const uint32_t i = s_dirty[k];
        free(s_base[i]);
        s_base[i] = s_cur[i];
        s_cur[i] = NULL;
    }
    s_n_dirty = 0;
}

void ramdisk_restore(void)
{
    for (uint32_t k = 0; k < s_n_dirty; k++) {
        const uint32_t i = s_dirty[k];
        free(s_cur[i]);
        s_cur[i] = NULL;
    }
    s_n_dirty = 0;
    ramdisk_power_on();
    s_writes = 0;
}

void ramdisk_cut_after(uint32_t whole, ramdisk_tear_t tear)
{
    s_arm = ARM_CUT;
    s_left = whole;
    s_tear = tear;
}

void ramdisk_fail_after(uint32_t whole)
{
    s_arm = ARM_FAIL;
    s_left = whole;
}

void ramdisk_power_on(void)
{
    s_arm = ARM_NONE;
    s_off = false;
    s_failing = false;
}

uint32_t ramdisk_writes(void)
{
    return s_writes;
}

void ramdisk_reset_count(void)
{
    s_writes = 0;
}

static DSTATUS ram_init(unsigned char pdrv)
{
    return 0;
}

static DRESULT ram_read(unsigned char pdrv, unsigned char *buf, uint32_t sector, UINT count)
{
    if (s_off) {
        return RES_NOTRDY;
    }
    if (sector >= s_sectors || count > s_sectors - sector) {
        return RES_PARERR;
    }
    for (UINT i = 0; i < count; i++) {
        memcpy(buf + (size_t)i * RAMDISK_SECTOR, ramdisk_sector(sector + i), RAMDISK_SECTOR);
    }
    return RES_OK;
}

// Lands bytes [from, to) of src in the sector.
static void land(uint32_t lba, const uint8_t *src, size_t from, size_t to)
{
    if (!s_cur[lba]) {
        const uint8_t *old = ramdisk_sector(lba);
        uint8_t *p = xmalloc(RAMDISK_SECTOR);
        memcpy(p, old, RAMDISK_SECTOR);
        s_cur[lba] = p;
        s_dirty[s_n_dirty++] = lba;
    }
    memcpy(s_cur[lba] + from, src + from, to - from);
    s_writes++;
}

static DRESULT ram_write(unsigned char pdrv, const unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_sectors || count > s_sectors - sector) {
        return RES_PARERR;
    }
    for (UINT i = 0; i < count; i++) {
        if (s_off) {
            return RES_NOTRDY;
        }
        if (s_failing) {
            return RES_ERROR;
        }
        const uint8_t *src = buf + (size_t)i * RAMDISK_SECTOR;
        if (s_arm != ARM_NONE) {
            if (s_left == 0) {
                if (s_arm == ARM_FAIL) {
                    s_failing = true;
                    return RES_ERROR;
                }
                s_off = true;
                if (s_tear == RAMDISK_TEAR_HEAD) {
                    land(sector + i, src, 0, RAMDISK_TEAR);
                } else if (s_tear == RAMDISK_TEAR_TAIL) {
                    land(sector + i, src, RAMDISK_SECTOR - RAMDISK_TEAR, RAMDISK_SECTOR);
                }
                return RES_NOTRDY;
            }
            s_left--;
        }
        land(sector + i, src, 0, RAMDISK_SECTOR);
    }
    return RES_OK;
}

static DRESULT ram_ioctl(unsigned char pdrv, unsigned char cmd, void *buf)
{
    if (s_off) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = s_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = RAMDISK_SECTOR;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

const ff_diskio_impl_t ramdisk_impl = {
    .init = ram_init,
    .status = ram_init,
    .read = ram_read,
    .write = ram_write,
    .ioctl = ram_ioctl,
};
