#pragma once

// A RAM disk for FatFs that counts sector writes and can cut the power. A cut lets a given number of sector writes
// land whole, optionally lands the next one torn (only its first or only its last 256 bytes new), and then fails
// every read and write until ramdisk_power_on, as a board that lost power would. It can also fail writes while the
// power stays on, as a card that reports a write error does.
//
// It's sparse: a sector nobody wrote holds a pseudo-random pattern made from its number, so a disk big enough for
// FAT32 with 4 KiB clusters costs only the sectors FatFs touches. The snapshot and every write since it are kept per
// sector, so a restore copies nothing.

#include <stdbool.h>
#include <stdint.h>

#include "diskio_impl.h"

#define RAMDISK_SECTOR 512
#define RAMDISK_TEAR 256 // bytes of a torn sector that land

typedef enum { RAMDISK_WHOLE, RAMDISK_TEAR_HEAD, RAMDISK_TEAR_TAIL } ramdisk_tear_t;

extern const ff_diskio_impl_t ramdisk_impl;

// A new disk of this many sectors, each holding pseudo-random bytes from seed, as a used card's free clusters hold
// old data. Frees the previous one.
void ramdisk_create(uint32_t sectors, uint32_t seed);
uint32_t ramdisk_sectors(void);
const uint8_t *ramdisk_sector(uint32_t lba);

// The image ramdisk_restore goes back to.
void ramdisk_snapshot(void);
// Back to the snapshot, power on, no cut or failure armed, write count 0.
void ramdisk_restore(void);

// Powers off at the write after `whole` more sector writes land; with a tear, that write lands torn first.
void ramdisk_cut_after(uint32_t whole, ramdisk_tear_t tear);
// From the write after `whole` more land, every write fails with RES_ERROR and lands nothing, power on.
void ramdisk_fail_after(uint32_t whole);
// Powers on and disarms a cut or a failure, keeping the disk as it is.
void ramdisk_power_on(void);

uint32_t ramdisk_writes(void); // sector writes that landed (whole or torn) since the last restore or count reset
void ramdisk_reset_count(void);
