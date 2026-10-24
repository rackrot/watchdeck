#include "ramdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *s_disk;
static uint8_t *s_base;  // the snapshot
static uint8_t *s_dirty; // a sector differs from the snapshot, so a restore copies it back
static uint32_t s_sectors;
static uint32_t s_writes;
static bool s_armed;
static uint32_t s_whole_left;
static bool s_tear;
static bool s_off;

void ramdisk_create(uint32_t sectors)
{
    s_sectors = sectors;
    s_disk = calloc(sectors, RAMDISK_SECTOR);
    s_base = calloc(sectors, RAMDISK_SECTOR);
    s_dirty = calloc(sectors, 1);
    if (!s_disk || !s_base || !s_dirty) {
        printf("ramdisk: no memory for %lu sectors\n", (unsigned long)sectors);
        exit(1);
    }
}

uint32_t ramdisk_sectors(void)
{
    return s_sectors;
}

const uint8_t *ramdisk_sector(uint32_t lba)
{
    return s_disk + (size_t)lba * RAMDISK_SECTOR;
}

void ramdisk_fill(uint32_t seed)
{
    uint32_t x = seed | 1;
    for (size_t i = 0; i < (size_t)s_sectors * RAMDISK_SECTOR; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s_disk[i] = (uint8_t)x;
    }
    memset(s_dirty, 1, s_sectors);
}

void ramdisk_snapshot(void)
{
    memcpy(s_base, s_disk, (size_t)s_sectors * RAMDISK_SECTOR);
    memset(s_dirty, 0, s_sectors);
}

void ramdisk_restore(void)
{
    for (uint32_t i = 0; i < s_sectors; i++) {
        if (s_dirty[i]) {
            memcpy(s_disk + (size_t)i * RAMDISK_SECTOR, s_base + (size_t)i * RAMDISK_SECTOR, RAMDISK_SECTOR);
            s_dirty[i] = 0;
        }
    }
    ramdisk_power_on();
    s_writes = 0;
}

void ramdisk_cut_after(uint32_t whole, bool tear)
{
    s_armed = true;
    s_whole_left = whole;
    s_tear = tear;
}

void ramdisk_power_on(void)
{
    s_armed = false;
    s_off = false;
}

bool ramdisk_is_off(void)
{
    return s_off;
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
    memcpy(buf, s_disk + (size_t)sector * RAMDISK_SECTOR, (size_t)count * RAMDISK_SECTOR);
    return RES_OK;
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
        size_t land = RAMDISK_SECTOR;
        if (s_armed) {
            if (s_whole_left == 0) {
                s_off = true;
                if (!s_tear) {
                    return RES_NOTRDY;
                }
                land = RAMDISK_TEAR;
            } else {
                s_whole_left--;
            }
        }
        memcpy(s_disk + (size_t)(sector + i) * RAMDISK_SECTOR, buf + (size_t)i * RAMDISK_SECTOR, land);
        s_dirty[sector + i] = 1;
        s_writes++;
    }
    return s_off ? RES_NOTRDY : RES_OK;
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
