#pragma once

// A RAM disk for FatFs that counts sector writes and can cut the power. A cut lets a given number of sector writes
// land whole, optionally lands the next one torn (its first 256 bytes new, the rest old), and then fails every read
// and write until ramdisk_restore, as a board that lost power would.

#include <stdbool.h>
#include <stdint.h>

#include "diskio_impl.h"

#define RAMDISK_SECTOR 512
#define RAMDISK_TEAR 256 // bytes of a torn sector that land

extern const ff_diskio_impl_t ramdisk_impl;

void ramdisk_create(uint32_t sectors);
uint32_t ramdisk_sectors(void);
const uint8_t *ramdisk_sector(uint32_t lba);

// Fills every sector with pseudo-random bytes, as a used card's free clusters hold old data.
void ramdisk_fill(uint32_t seed);
// The image ramdisk_restore goes back to.
void ramdisk_snapshot(void);
// Back to the snapshot, power on, no cut armed, write count 0.
void ramdisk_restore(void);

// Powers off at the write after `whole` more sector writes land; with tear, that write lands torn first.
void ramdisk_cut_after(uint32_t whole, bool tear);
// Powers on and disarms the cut, keeping the disk as the cut left it.
void ramdisk_power_on(void);
bool ramdisk_is_off(void);

uint32_t ramdisk_writes(void); // sector writes that landed (whole or torn) since the last restore or count reset
void ramdisk_reset_count(void);
