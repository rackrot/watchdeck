#include "cardmkfs.h"

#include <string.h>

#include "ff.h"

#define MBR_SIG_OFS 510
#define MBR_ID_OFS 440    // the disk signature
#define MBR_TABLE_OFS 446 // the first partition entry
#define PART_TYPE_FAT32_LBA 0x0C
#define CHS_SECTORS 63
#define CHS_HEADS 255
#define VBR_BACKUP 6 // f_mkfs's backup boot sector, and FSINFO's backup one after it
#define VBR_FSTYPE_OFS 82

static const uint64_t GIB = 1024ull * 1024 * 1024;

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool cardmkfs_layout(uint32_t capacity, cardmkfs_layout_t *out)
{
    if (capacity < CARDMKFS_MIN_SECTORS) {
        return false;
    }
    out->part_lba = CARDMKFS_PART_LBA;
    out->part_sectors = capacity - CARDMKFS_PART_LBA;
    const uint64_t bytes = (uint64_t)out->part_sectors * CARDMKFS_SECTOR;
    out->cluster_bytes = bytes < 8 * GIB ? 4096 : bytes < 16 * GIB ? 8192 : bytes < 32 * GIB ? 16384 : 32768;
    return true;
}

// The 3-byte CHS address of lba, as FatFs's create_partition writes it.
static void put_chs(uint8_t *p, uint32_t lba)
{
    const uint32_t cyl = lba / (CHS_HEADS * CHS_SECTORS);
    if (cyl > 1023) {
        p[0] = 0xFE;
        p[1] = 0xFF;
        p[2] = 0xFF;
        return;
    }
    p[0] = (uint8_t)(lba / CHS_SECTORS % CHS_HEADS);
    p[1] = (uint8_t)(lba % CHS_SECTORS + 1) | (uint8_t)(cyl >> 2 & 0xC0);
    p[2] = (uint8_t)cyl;
}

void cardmkfs_mbr(const cardmkfs_layout_t *layout, uint32_t disk_id, uint8_t out[CARDMKFS_SECTOR])
{
    memset(out, 0, CARDMKFS_SECTOR);
    put_le32(out + MBR_ID_OFS, disk_id);
    uint8_t *pte = out + MBR_TABLE_OFS;
    pte[0] = 0x00; // not bootable
    put_chs(pte + 1, layout->part_lba);
    pte[4] = PART_TYPE_FAT32_LBA;
    put_chs(pte + 5, layout->part_lba + layout->part_sectors - 1);
    put_le32(pte + 8, layout->part_lba);
    put_le32(pte + 12, layout->part_sectors);
    out[MBR_SIG_OFS] = 0x55;
    out[MBR_SIG_OFS + 1] = 0xAA;
}

const char *cardmkfs_step_text(cardmkfs_step_t step)
{
    switch (step) {
    case CARDMKFS_STEP_LAYOUT:
        return "sizing the partition";
    case CARDMKFS_STEP_WIPE:
        return "clearing the old partition table";
    case CARDMKFS_STEP_MKFS:
        return "creating the file system";
    case CARDMKFS_STEP_CHECK:
        return "checking the file system";
    case CARDMKFS_STEP_MBR:
        return "writing the partition table";
    default:
        return "done";
    }
}

static esp_err_t zero_range(const cardmkfs_io_t *io, uint32_t first, uint32_t count, uint8_t *work)
{
    memset(work, 0, CARDMKFS_SECTOR);
    for (uint32_t i = 0; i < count; i++) {
        const esp_err_t err = io->write(first + i, work, io->ctx);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

// f_mkfs writes the backup boot sector and both FSINFO sectors without checking the result, so read the pairs back.
static esp_err_t check_boot_sectors(const cardmkfs_io_t *io, uint32_t part_lba, uint8_t *work)
{
    uint8_t *a = work, *b = work + CARDMKFS_SECTOR;
    for (uint32_t s = 0; s < 2; s++) { // the boot sector, then FSINFO
        esp_err_t err = io->read(part_lba + s, a, io->ctx);
        if (err == ESP_OK) {
            err = io->read(part_lba + VBR_BACKUP + s, b, io->ctx);
        }
        if (err != ESP_OK) {
            return err;
        }
        if (memcmp(a, b, CARDMKFS_SECTOR) != 0 || a[MBR_SIG_OFS] != 0x55 || a[MBR_SIG_OFS + 1] != 0xAA ||
            (s == 0 && memcmp(a + VBR_FSTYPE_OFS, "FAT32   ", 8) != 0)) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

esp_err_t cardmkfs_run(const cardmkfs_io_t *io, const char *drive, uint32_t capacity, uint32_t disk_id, uint8_t *work,
                       uint32_t work_len, cardmkfs_step_t *step, int *fresult)
{
    *fresult = FR_OK;
    *step = CARDMKFS_STEP_LAYOUT;
    cardmkfs_layout_t layout;
    if (work_len < 2 * CARDMKFS_SECTOR || !cardmkfs_layout(capacity, &layout)) {
        return ESP_ERR_INVALID_SIZE;
    }

    *step = CARDMKFS_STEP_WIPE;
    esp_err_t err = zero_range(io, 0, CARDMKFS_HEAD_SECTORS, work);
    if (err == ESP_OK) {
        err = zero_range(io, capacity - CARDMKFS_GPT_TAIL, CARDMKFS_GPT_TAIL, work);
    }
    if (err != ESP_OK) {
        return err;
    }

    *step = CARDMKFS_STEP_MKFS;
    io->set_window(layout.part_lba, layout.part_sectors, io->ctx);
    // FM_SFD: no table of f_mkfs's own (the MBR below is ours). VolToPart's pt 0 with FM_SFD formats the drive's
    // whole window from its sector 0.
    const MKFS_PARM opt = {
        .fmt = FM_FAT32 | FM_SFD,
        .n_fat = 2,
        .align = CARDMKFS_ALIGN,
        .au_size = layout.cluster_bytes,
    };
    *fresult = f_mkfs(drive, &opt, work, work_len);
    if (*fresult != FR_OK) {
        return ESP_FAIL;
    }

    *step = CARDMKFS_STEP_CHECK;
    err = check_boot_sectors(io, layout.part_lba, work);
    if (err != ESP_OK) {
        return err;
    }

    *step = CARDMKFS_STEP_MBR;
    cardmkfs_mbr(&layout, disk_id, work);
    err = io->write(0, work, io->ctx);
    if (err != ESP_OK) {
        return err;
    }
    *step = CARDMKFS_STEP_DONE;
    return ESP_OK;
}
