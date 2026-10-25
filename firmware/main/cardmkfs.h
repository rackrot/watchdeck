#pragma once

// What Format card writes, with no card or task state, so the host test runs the same code over a disk image. The only
// file allowed to call f_mkfs, which the no-card-format prek hook enforces.
//
// The layout: one MBR partition, type 0x0C (FAT32, LBA), from CARDMKFS_PART_LBA to the card's last sector, holding a
// FAT32 volume with 2 FATs, its data area aligned to CARDMKFS_ALIGN sectors from the partition's start, and the
// cluster size cardmkfs_layout picks. FAT32 only: IDF's FatFs has no exFAT (ffconf.h FF_FS_EXFAT 0) and cardfs_fat.c
// walks FAT32 only. 32-bit LBAs reach 2 TiB.
//
// The order of cardmkfs_run keeps a cut safe: LBA 0 (any MBR or GPT's protective MBR), the primary GPT (LBA 1 to 33)
// and the backup GPT (the last 33 sectors) are zeroed first, then f_mkfs fills the partition, then its boot sectors
// are checked, and the MBR goes last. Until that one sector lands the card has no partition table and no file system
// at LBA 0, so a cut anywhere before leaves a card nothing mounts (NO_CARD; format it again), never an old table
// pointing at a half-made volume.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CARDMKFS_SECTOR 512
// Both 4 MiB, an SD erase-block (allocation unit) boundary: the partition starts at CARDMKFS_PART_LBA, and f_mkfs
// aligns the data area CARDMKFS_ALIGN sectors from the partition start, so the data area and every cluster sit 4 MiB
// aligned on the card.
#define CARDMKFS_PART_LBA 8192
#define CARDMKFS_ALIGN 8192
#define CARDMKFS_HEAD_SECTORS 34       // LBA 0 to 33: the MBR or protective MBR, the GPT header and its entries
#define CARDMKFS_GPT_TAIL 33           // the backup GPT: its entries and, in the last sector, its header
#define CARDMKFS_MIN_SECTORS 0x100000u // 512 MiB: below this FAT32's 65,526 clusters don't fit at 4 KiB

typedef struct {
    uint32_t part_lba;      // CARDMKFS_PART_LBA
    uint32_t part_sectors;  // to the card's end
    uint32_t cluster_bytes; // 4, 8, 16 or 32 KiB
} cardmkfs_layout_t;

// The layout for a card of `capacity` 512-byte sectors. Cluster size by partition size: under 8 GiB 4 KiB, under
// 16 GiB 8 KiB, under 32 GiB 16 KiB, else 32 KiB (Microsoft's FAT32 defaults). False: smaller than
// CARDMKFS_MIN_SECTORS.
bool cardmkfs_layout(uint32_t capacity, cardmkfs_layout_t *out);

// The 512-byte MBR for the layout: no boot code, disk signature disk_id, and one entry, not bootable, type 0x0C, with
// CHS addresses for 255 heads and 63 sectors (0xFE 0xFF 0xFF past cylinder 1023).
void cardmkfs_mbr(const cardmkfs_layout_t *layout, uint32_t disk_id, uint8_t out[CARDMKFS_SECTOR]);

// The card, as the caller has it. Every buffer passed to read and write is the caller's work buffer.
typedef struct {
    // One sector at a card LBA (not inside the window). A single-block write returns its own error.
    esp_err_t (*write)(uint32_t lba, const uint8_t *buf, void *ctx);
    esp_err_t (*read)(uint32_t lba, uint8_t *buf, void *ctx);
    // Points the FatFs diskio of `drive` at card sectors [first, first + sectors).
    void (*set_window)(uint32_t first, uint32_t sectors, void *ctx);
    void *ctx;
} cardmkfs_io_t;

// Where cardmkfs_run stopped, for the message that says so.
typedef enum {
    CARDMKFS_STEP_LAYOUT, // the card is too small
    CARDMKFS_STEP_WIPE,   // zeroing the old tables
    CARDMKFS_STEP_MKFS,   // f_mkfs
    CARDMKFS_STEP_CHECK,  // reading back the boot sectors f_mkfs wrote unchecked
    CARDMKFS_STEP_MBR,    // writing the partition table
    CARDMKFS_STEP_DONE,
} cardmkfs_step_t;

const char *cardmkfs_step_text(cardmkfs_step_t step); // "clearing the old partition table", ...

// Formats a card of `capacity` sectors in the order above. `drive` is a FatFs drive ("1:") registered with a diskio
// that io->set_window moves; nothing may be mounted on it. work is at least two sectors (64-byte aligned DMA memory on
// the board); f_mkfs fills the FAT that many sectors per write, so bigger is faster. On return *step says how far it
// got (CARDMKFS_STEP_DONE on ESP_OK) and *fresult holds f_mkfs's result. The window is left on the partition.
// ESP_ERR_INVALID_SIZE: smaller than CARDMKFS_MIN_SECTORS, nothing written. ESP_ERR_INVALID_CRC: a boot sector read
// back differs from its backup. Otherwise the io's or ESP_FAIL (f_mkfs).
esp_err_t cardmkfs_run(const cardmkfs_io_t *io, const char *drive, uint32_t capacity, uint32_t disk_id, uint8_t *work,
                       uint32_t work_len, cardmkfs_step_t *step, int *fresult);
