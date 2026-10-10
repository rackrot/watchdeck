#include "sdcard.h"

#include <stdio.h>
#include <string.h>

#include "diskio_impl.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#define SD_CLK_GPIO 43
#define SD_CMD_GPIO 44
#define SD_D0_GPIO 39

static const char *TAG = "sdcard";

// ESP-IDF's FatFs is built without 64-bit LBA, so it cannot see inside a GPT disk
// (the card ships GPT-partitioned). We read the first GPT entry ourselves and expose
// that partition to FatFs through a read-only, offset diskio driver.
// ponytail: one card, one partition, so the driver state is static.
static sdmmc_card_t *s_card;
static uint32_t s_part_first_lba;
static uint32_t s_part_sectors;
static char s_drive[3];
static bool s_card_ready; // sdmmc_card_init succeeded

static DSTATUS part_disk_init(unsigned char pdrv)
{
    return STA_PROTECT; // read-only: FatFs rejects writes up front
}

static DRESULT part_disk_read(unsigned char pdrv, unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_part_sectors || count > s_part_sectors - sector) {
        return RES_PARERR;
    }
    esp_err_t err = sdmmc_read_sectors(s_card, buf, s_part_first_lba + sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read sector %lu x%u: %s", (unsigned long)sector, count, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT part_disk_write(unsigned char pdrv, const unsigned char *buf, uint32_t sector, UINT count)
{
    return RES_WRPRT; // this firmware never writes to the card
}

static DRESULT part_disk_ioctl(unsigned char pdrv, unsigned char cmd, void *buf)
{
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buf = s_part_sectors;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = s_card->csd.sector_size;
        return RES_OK;
    default:
        return RES_ERROR;
    }
}

static const ff_diskio_impl_t s_part_impl = {
    .init = part_disk_init,
    .status = part_disk_init,
    .read = part_disk_read,
    .write = part_disk_write,
    .ioctl = part_disk_ioctl,
};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

// Locate the first GPT partition. A disk without a GPT header starts at LBA 0 and
// FatFs parses any MBR itself.
static esp_err_t find_partition(sdmmc_card_t *card)
{
    uint8_t *buf = heap_caps_aligned_alloc(64, 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no mem");

    s_part_first_lba = 0;
    s_part_sectors = card->csd.capacity;

    esp_err_t err = sdmmc_read_sectors(card, buf, 1, 1);
    if (err == ESP_OK && memcmp(buf, "EFI PART", 8) == 0) {
        uint64_t entries_lba = le32(buf + 72) | (uint64_t)le32(buf + 76) << 32;
        if (entries_lba == 0 || entries_lba >= card->csd.capacity) {
            ESP_LOGE(TAG, "GPT entry table LBA out of range");
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            err = sdmmc_read_sectors(card, buf, (size_t)entries_lba, 1);
        }
        if (err == ESP_OK) {
            uint64_t first = le32(buf + 32) | (uint64_t)le32(buf + 36) << 32;
            uint64_t last = le32(buf + 40) | (uint64_t)le32(buf + 44) << 32;
            if (first == 0 || first > last || last >= card->csd.capacity) {
                ESP_LOGE(TAG, "unusable first GPT entry");
                err = ESP_ERR_NOT_SUPPORTED;
            } else {
                s_part_first_lba = first;
                s_part_sectors = last - first + 1;
                ESP_LOGI(TAG, "GPT partition 1: lba %lu, %lu sectors", (unsigned long)first,
                         (unsigned long)s_part_sectors);
            }
        }
    }
    free(buf);
    return err;
}

static esp_err_t mount_card(void)
{
    // Share the host with ESP-Hosted: init is idempotent and we never deinit.
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; // 20 MHz; Elecrow uses 10 MHz

    // Slot 0 is 1-bit only on this board, with no card-detect line.
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = SD_CLK_GPIO;
    slot.cmd = SD_CMD_GPIO;
    slot.d0 = SD_D0_GPIO;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "slot init");

    s_card = calloc(1, sizeof(*s_card));
    ESP_RETURN_ON_FALSE(s_card, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_ERROR(sdmmc_card_init(&host, s_card), TAG, "card init");
    s_card_ready = true;
    ESP_RETURN_ON_ERROR(find_partition(s_card), TAG, "partition lookup");

    BYTE pdrv;
    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(&pdrv), TAG, "no free drive");
    ff_diskio_register(pdrv, &s_part_impl);

    char *drive = s_drive;
    drive[0] = '0' + pdrv;
    drive[1] = ':';
    drive[2] = '\0';
    esp_vfs_fat_conf_t conf = {.base_path = SDCARD_MOUNT_POINT, .fat_drive = drive, .max_files = 4};
    FATFS *fs;
    esp_err_t err = esp_vfs_fat_register(&conf, &fs);
    if (err == ESP_OK) {
        FRESULT res = f_mount(fs, drive, 1); // never f_mkfs: the card holds user data
        if (res != FR_OK) {
            ESP_LOGE(TAG, "f_mount failed: FRESULT %d", res);
            esp_vfs_fat_unregister_path(SDCARD_MOUNT_POINT);
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        ff_diskio_unregister(pdrv);
        s_drive[0] = '\0';
    }
    return err;
}

esp_err_t sdcard_mount(char *msg, size_t msg_len)
{
    esp_err_t err = mount_card();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        snprintf(msg, msg_len, "SD: not mounted (%s)", esp_err_to_name(err));
        return err;
    }

    // Capacity only: scanning the FAT for free space on 128 GB is too slow.
    double gb = (double)s_card->csd.capacity * s_card->csd.sector_size / (1024.0 * 1024.0 * 1024.0);
    ESP_LOGI(TAG, "mounted %s: %s, %.1f GB", SDCARD_MOUNT_POINT, s_card->cid.name, gb);
    snprintf(msg, msg_len, "SD: %.1f GB", gb);
    return ESP_OK;
}

sdmmc_card_t *sdcard_card(void)
{
    return s_card_ready ? s_card : NULL;
}

void sdcard_release_fs(void)
{
    if (s_drive[0] == '\0') {
        return;
    }
    f_mount(NULL, s_drive, 0);
    esp_err_t err = esp_vfs_fat_unregister_path(SDCARD_MOUNT_POINT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vfs unregister %s: %s", SDCARD_MOUNT_POINT, esp_err_to_name(err));
    }
    ff_diskio_unregister((BYTE)(s_drive[0] - '0'));
    s_drive[0] = '\0';
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "released %s; card handed off", SDCARD_MOUNT_POINT);
    }
}
