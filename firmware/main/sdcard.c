#include "sdcard.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cardfs.h"
#include "cardmkfs.h"
#include "diag.h"
#include "diskio_impl.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "svc.h"

#define SD_CLK_GPIO 43
#define SD_CMD_GPIO 44
#define SD_D0_GPIO 39
#define TEXT_MAX 64
#define VERIFY_SECTORS 8 // the read-back of a multi-block write goes in pieces this big (4 KiB)
#define GPT_RESERVED 33  // sectors the GPT keeps at the end of the disk (backup entries and header)

static const char *TAG = "sdcard";

// ESP-IDF's FatFs is built without 64-bit LBA, so it cannot see inside a GPT disk
// (the card ships GPT-partitioned). We read the first GPT entry ourselves and expose
// that partition to FatFs through an offset diskio driver that refuses any sector outside it.
// A card Format card wrote (cardmkfs.h) has an MBR instead, so the window is the whole card and FatFs finds the
// partition in the MBR itself.
// ponytail: one card, one partition, so the driver state is static.
static sdmmc_card_t *s_card;
static uint32_t s_part_first_lba;
static uint32_t s_part_sectors;
static char s_drive[3];     // the FatFs drive, "0:"; empty while unmounted. cardfs keeps a pointer to it
static bool s_card_ready;   // sdmmc_card_init succeeded
static bool s_store_inited; // cardfs_init has run (a mount succeeded once), so cardfs_attach and _detach work
static double s_gb;
static volatile sdcard_owner_t s_owner = SDCARD_NO_CARD;
static const char *volatile s_fs_name; // of the first partition once it mounted; a literal, so a read can't tear
static volatile esp_err_t s_error;     // why the owner is NO_CARD
static char s_text[TEXT_MAX];
static sdcard_owner_cb_t s_owner_cb;
static uint8_t *s_verify_buf; // VERIFY_SECTORS sectors of internal DMA RAM, from the first mount on
static bool s_verify_logged;
static volatile uint64_t s_verify_sectors, s_verify_us;
// The first write error part_disk_write returned since the format set the window: FatFs's mkfs ignores some of its own.
static esp_err_t s_window_err;

static const cardfs_volume_t s_volume = {
    .prepare = cardfs_fat_prepare,
    .room = cardfs_fat_room,
    .space = cardfs_fat_space,
    .ctx = s_drive,
};

static DSTATUS part_disk_init(unsigned char pdrv)
{
    return 0; // ready and writable; cardfs.c decides what is written
}

static DRESULT part_disk_read(unsigned char pdrv, unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_part_sectors || count > s_part_sectors - sector) {
        return RES_PARERR;
    }
    esp_err_t err = sdmmc_read_sectors(s_card, buf, s_part_first_lba + sector, count);
    if (err != ESP_OK) {
        diag_count(DIAG_CARD_ERROR, esp_err_to_name(err));
        ESP_LOGE(TAG, "read sector %lu x%u: %s", (unsigned long)sector, count, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

// Reads back a multi-block write and compares it, because IDF 6.1 can report a failed one as written:
// sdmmc_write_sectors_dma (components/sdmmc/sdmmc_cmd.c:578-601) overwrites the write's error with the result of
// its ACMD22 "how many blocks were written" query and returns that, so a failed CMD25 whose ACMD22 succeeds returns
// ESP_OK. A single block (CMD24) returns its own error, so it isn't read back. Drop this once upstream returns the
// write's error. Runs under FatFs's volume lock (or in the format, whose drive is the only one), the only user of
// s_verify_buf. first is a card LBA.
static DRESULT verify_written(const unsigned char *buf, size_t first, UINT count)
{
    const int64_t start = esp_timer_get_time();
    const size_t ss = s_card->csd.sector_size;
    for (UINT done = 0; done < count;) {
        const UINT n = count - done < VERIFY_SECTORS ? count - done : VERIFY_SECTORS;
        const size_t lba = first + done;
        esp_err_t err = sdmmc_read_sectors(s_card, s_verify_buf, lba, n);
        if (err == ESP_OK && memcmp(s_verify_buf, buf + (size_t)done * ss, n * ss) != 0) {
            err = ESP_ERR_INVALID_CRC;
        }
        if (err != ESP_OK) {
            diag_count(DIAG_CARD_ERROR, err == ESP_ERR_INVALID_CRC ? "a write didn't reach the card" : "write check");
            if (!s_verify_logged) {
                s_verify_logged = true;
                ESP_LOGE(TAG, "write check at card lba %u (of %u x%u): %s", (unsigned)lba, (unsigned)first, count,
                         err == ESP_ERR_INVALID_CRC ? "the card holds other bytes" : esp_err_to_name(err));
            }
            return RES_ERROR;
        }
        done += n;
    }
    s_verify_sectors += count;
    s_verify_us += (uint64_t)(esp_timer_get_time() - start);
    return RES_OK;
}

static DRESULT part_disk_write(unsigned char pdrv, const unsigned char *buf, uint32_t sector, UINT count)
{
    if (sector >= s_part_sectors || count > s_part_sectors - sector) {
        return RES_PARERR; // never outside the partition
    }
    // sdmmc_write_sectors returns once the card has finished programming, so CTRL_SYNC has nothing to wait for.
    esp_err_t err = sdmmc_write_sectors(s_card, buf, s_part_first_lba + sector, count);
    if (err != ESP_OK) {
        diag_count(DIAG_CARD_ERROR, esp_err_to_name(err));
        ESP_LOGE(TAG, "write sector %lu x%u: %s", (unsigned long)sector, count, esp_err_to_name(err));
        s_window_err = s_window_err != ESP_OK ? s_window_err : err;
        return RES_ERROR;
    }
    const DRESULT res = count > 1 ? verify_written(buf, s_part_first_lba + sector, count) : RES_OK;
    if (res != RES_OK && s_window_err == ESP_OK) {
        s_window_err = ESP_ERR_INVALID_CRC;
    }
    return res;
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
        const uint64_t capacity = card->csd.capacity;
        const uint64_t entries_lba = le32(buf + 72) | (uint64_t)le32(buf + 76) << 32;
        // The entry array's length: entry count times entry size, in whole sectors.
        const uint64_t entries_end = entries_lba + ((uint64_t)le32(buf + 80) * le32(buf + 84) + 511) / 512;
        if (entries_lba < 2 || entries_lba >= capacity) {
            ESP_LOGE(TAG, "GPT entry table LBA out of range");
            err = ESP_ERR_NOT_SUPPORTED;
        } else {
            err = sdmmc_read_sectors(card, buf, (size_t)entries_lba, 1);
        }
        if (err == ESP_OK) {
            uint64_t first = le32(buf + 32) | (uint64_t)le32(buf + 36) << 32;
            uint64_t last = le32(buf + 40) | (uint64_t)le32(buf + 44) << 32;
            // Never over the GPT itself: the primary header and entries before it, the backup ones after it.
            if (first < 34 || first < entries_end || first > last || capacity <= GPT_RESERVED ||
                last >= capacity - GPT_RESERVED) {
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

static esp_err_t init_card(void)
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
    // Before the partition lookup, so a card whose table is refused still has them for Format card.
    s_gb = (double)s_card->csd.capacity * s_card->csd.sector_size / (1024.0 * 1024.0 * 1024.0);
    s_verify_buf =
        heap_caps_aligned_alloc(64, VERIFY_SECTORS * s_card->csd.sector_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_verify_buf, ESP_ERR_NO_MEM, TAG, "no mem for the write check");
    ESP_RETURN_ON_ERROR(find_partition(s_card), TAG, "partition lookup");
    return ESP_OK;
}

// Registers the diskio and the VFS path and mounts the partition. The file system is never created here: a card
// FatFs can't mount stays unmounted, since it holds user data.
static esp_err_t mount_fs(void)
{
    BYTE pdrv;
    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(&pdrv), TAG, "no free drive");
    ff_diskio_register(pdrv, &s_part_impl);

    s_drive[0] = '0' + pdrv;
    s_drive[1] = ':';
    s_drive[2] = '\0';
    esp_vfs_fat_conf_t conf = {.base_path = SDCARD_MOUNT_POINT, .fat_drive = s_drive, .max_files = 4};
    FATFS *fs;
    esp_err_t err = esp_vfs_fat_register(&conf, &fs);
    if (err == ESP_OK) {
        FRESULT res = f_mount(fs, s_drive, 1);
        if (res != FR_OK) {
            ESP_LOGE(TAG, "f_mount failed: FRESULT %d", res);
            f_mount(NULL, s_drive, 0); // FatFs keeps the FATFS registered even when the mount fails
            esp_vfs_fat_unregister_path(SDCARD_MOUNT_POINT);
            err = ESP_FAIL;
        } else {
            static const char *const FS_NAMES[] = {NULL, "FAT12", "FAT16", "FAT32", "exFAT"};
            s_fs_name = fs->fs_type < sizeof(FS_NAMES) / sizeof(FS_NAMES[0]) ? FS_NAMES[fs->fs_type] : NULL;
            ESP_LOGI(TAG, "%s volume at card lba %lu", s_fs_name ? s_fs_name : "FAT",
                     (unsigned long)(s_part_first_lba + fs->volbase));
        }
    }
    if (err != ESP_OK) {
        ff_diskio_unregister(pdrv);
        s_drive[0] = '\0';
        s_fs_name = NULL;
    }
    return err;
}

// Drops FatFs's mount, with anything it still caches, and the VFS registration. No-op if nothing is mounted.
static void unmount_fs(void)
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
    ESP_LOGI(TAG, "unmounted %s", SDCARD_MOUNT_POINT);
}

static esp_err_t remount(void);
static void set_state(sdcard_owner_t owner, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_state(sdcard_owner_t owner, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_text, sizeof(s_text), fmt, args);
    va_end(args);
    s_owner = owner;
    if (owner != SDCARD_NO_CARD) {
        s_error = ESP_OK;
    }
    ESP_LOGI(TAG, "%s", s_text);
    if (s_owner_cb) {
        s_owner_cb(owner, s_text);
    }
}

// The SD text for a mounted card, from what cardfs allows.
static void set_mounted(esp_err_t why_not_writable)
{
    if (cardfs_writable() && !cardfs_tainted()) {
        set_state(SDCARD_DEVICE, "SD: %.1f GB", s_gb);
    } else {
        const char *why = why_not_writable == ESP_ERR_NOT_SUPPORTED ? "not FAT32"
                          : why_not_writable != ESP_OK              ? esp_err_to_name(why_not_writable)
                                                                    : "card error";
        set_state(SDCARD_DEVICE, "SD: %.1f GB, read-only (%s)", s_gb, why);
    }
}

// After a mount: turns cardfs writes on and resolves its intent records. Never fails the mount: a card cardfs
// can't write stays mounted read-only, and a recovery that fails leaves its record for the next mount. Returns why
// writes are off, or ESP_OK.
static esp_err_t open_store(bool first)
{
    esp_err_t err = first ? cardfs_init(SDCARD_MOUNT_POINT, &s_volume) : ESP_OK;
    s_store_inited = true;
    if (err == ESP_OK) {
        err = cardfs_set_writable(true);
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "the card isn't FAT32, so it stays read-only");
        return err;
    }
    if (err != ESP_OK) {
        // Not sdcard_note_failure: that remounts on a taint, and this runs inside a remount.
        ESP_LOGE(TAG, "card writes stay off: %s", esp_err_to_name(err));
        diag_count(DIAG_CARD_ERROR, "card writes off");
        return err;
    }
    err = cardfs_recover();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card recovery left a write for the next mount: %s", esp_err_to_name(err));
        diag_count(DIAG_CARD_ERROR, "recovery left for the next mount");
    }
    return ESP_OK;
}

esp_err_t sdcard_mount(char *msg, size_t msg_len)
{
    esp_err_t err = init_card();
    if (err == ESP_OK) {
        err = mount_fs();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        diag_count(DIAG_CARD_ERROR, "mount failed");
        s_error = err;
        set_state(SDCARD_NO_CARD, "SD: not mounted (%s)", esp_err_to_name(err));
        snprintf(msg, msg_len, "%s", s_text);
        return err;
    }
    ESP_LOGI(TAG, "mounted %s read-write: %s, %.1f GB", SDCARD_MOUNT_POINT, s_card->cid.name, s_gb);
    s_owner = SDCARD_DEVICE;
    const esp_err_t why = open_store(true);
    if (why == ESP_OK && cardfs_tainted()) {
        // Once, from app_main: no card job runs yet. It sets the state either way.
        ESP_LOGW(TAG, "a card access failed during recovery, mounting again");
        err = remount();
    } else {
        set_mounted(why);
    }
    snprintf(msg, msg_len, "%s", s_text);
    return err;
}

void sdcard_set_owner_cb(sdcard_owner_cb_t cb)
{
    s_owner_cb = cb;
}

sdcard_owner_t sdcard_owner(void)
{
    return s_owner;
}

sdmmc_card_t *sdcard_card(void)
{
    return s_card_ready ? s_card : NULL;
}

void sdcard_info(sdcard_info_t *out)
{
    *out = (sdcard_info_t){.present = s_card_ready, .fs = s_fs_name, .error = s_error};
    if (s_card_ready) {
        _Static_assert(sizeof(((sdcard_info_t *)0)->name) > sizeof(((sdmmc_cid_t *)0)->name), "room for the NUL");
        memcpy(out->name, s_card->cid.name, sizeof(s_card->cid.name)); // out is zeroed, so the NUL is there
        out->bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    }
}

void sdcard_write_check_stats(uint64_t *sectors, uint64_t *us)
{
    *sectors = s_verify_sectors;
    *us = s_verify_us;
}

// Unmounts and mounts again with cardfs detached in between, so no cardfs call is inside FatFs while f_mount(NULL)
// drops the volume. A failed mount leaves cardfs detached: its calls say ESP_ERR_INVALID_STATE.
static esp_err_t remount(void)
{
    if (s_owner != SDCARD_DEVICE) {
        return ESP_ERR_INVALID_STATE;
    }
    cardfs_detach(); // waits for a call in progress; clears the taint
    unmount_fs();
    esp_err_t err = mount_fs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remount failed: %s", esp_err_to_name(err));
        diag_count(DIAG_CARD_ERROR, "remount failed");
        s_error = err;
        set_state(SDCARD_NO_CARD, "SD: not mounted (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mounted %s again", SDCARD_MOUNT_POINT);
    cardfs_attach();
    set_mounted(open_store(false));
    return ESP_OK;
}

esp_err_t sdcard_remount(void)
{
    assert(svc_on_task());
    return remount();
}

void sdcard_note_failure(const char *what, esp_err_t err)
{
    assert(svc_on_task());
    const bool tainted = cardfs_tainted();
    if (err == ESP_ERR_INVALID_STATE && !tainted) {
        ESP_LOGW(TAG, "%s: the card isn't writable now (%s)", what, s_text);
        return; // shared, read-only or absent: a state, not a card error
    }
    if (err == CARDFS_ERR_DIR_FULL) {
        ESP_LOGE(TAG, "%s: the card's root or watchdeck/ is full; add or delete a file there on a PC", what);
        diag_count(DIAG_CARD_ERROR, "card folder full, tidy it on a PC");
    } else {
        ESP_LOGE(TAG, "%s: %s%s", what, esp_err_to_name(err), tainted ? " (card access failed)" : "");
        diag_count(DIAG_CARD_ERROR, tainted ? "card access failed" : esp_err_to_name(err));
    }
    if (tainted && s_owner == SDCARD_DEVICE) {
        ESP_LOGW(TAG, "mounting the card again so writes can go on");
        remount();
    }
}

esp_err_t sdcard_release_fs(void)
{
    assert(svc_on_task());
    if (s_owner != SDCARD_DEVICE) {
        return ESP_ERR_INVALID_STATE;
    }
    cardfs_detach(); // waits for a call in progress; every later one says ESP_ERR_INVALID_STATE
    unmount_fs();
    return ESP_OK;
}

void sdcard_set_shared(void)
{
    set_state(SDCARD_SHARED, "SD: shared over USB");
}

/* ---- Format card: the io cardmkfs_run writes the card through ---- */

// One block (CMD24), which returns its own error, so no read-back (verify_written) is needed.
static esp_err_t format_write(uint32_t lba, const uint8_t *buf, void *ctx)
{
    return lba < (uint32_t)s_card->csd.capacity ? sdmmc_write_sectors(s_card, buf, lba, 1) : ESP_ERR_INVALID_ARG;
}

static esp_err_t format_read(uint32_t lba, uint8_t *buf, void *ctx)
{
    return lba < (uint32_t)s_card->csd.capacity ? sdmmc_read_sectors(s_card, buf, lba, 1) : ESP_ERR_INVALID_ARG;
}

static void format_window(uint32_t first, uint32_t sectors, void *ctx)
{
    s_part_first_lba = first;
    s_part_sectors = sectors;
    s_window_err = ESP_OK;
}

static esp_err_t format_write_error(void *ctx)
{
    return s_window_err;
}

// The format's work buffer: CARDFS_PIECE first, so ESP-Hosted's SDIO traffic interleaves as it does with cardfs's
// writes.
static uint8_t *format_buffer(uint32_t *len)
{
    static const uint32_t SIZES[] = {CARDFS_PIECE, 8192, 1024};
    for (size_t i = 0; i < sizeof(SIZES) / sizeof(SIZES[0]); i++) {
        uint8_t *buf = heap_caps_aligned_alloc(64, SIZES[i], MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (buf) {
            *len = SIZES[i];
            return buf;
        }
    }
    return NULL;
}

// Registers a drive on the partition diskio and runs cardmkfs_run there. Nothing is mounted. *reached says whether
// anything was written.
static esp_err_t format_card(uint32_t capacity, uint8_t *work, uint32_t work_len, const char **step,
                             sdcard_format_reached_t *reached)
{
    *reached = SDCARD_FORMAT_UNTOUCHED;
    BYTE pdrv;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK) {
        *step = "finding a free FatFs drive";
        return err;
    }
    ff_diskio_register(pdrv, &s_part_impl);
    const char drive[3] = {(char)('0' + pdrv), ':', '\0'};
    const cardmkfs_io_t io = {
        .write = format_write, .read = format_read, .set_window = format_window, .write_error = format_write_error};
    cardmkfs_step_t at;
    int fresult;
    err = cardmkfs_run(&io, drive, capacity, esp_random(), work, work_len, &at, &fresult);
    ff_diskio_unregister(pdrv);
    *step = cardmkfs_step_text(at);
    *reached = at == CARDMKFS_STEP_START  ? SDCARD_FORMAT_UNTOUCHED
               : at == CARDMKFS_STEP_DONE ? SDCARD_FORMAT_WRITTEN
                                          : SDCARD_FORMAT_PARTIAL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format: %s failed: %s (FRESULT %d)", *step, esp_err_to_name(err), fresult);
    }
    return err;
}

// After a mount: cardfs attached (or initialised, the first time), writes on, recovery; the owner is DEVICE.
static esp_err_t attach_store(void)
{
    const bool first = !s_store_inited;
    if (!first) {
        cardfs_attach();
    }
    s_owner = SDCARD_DEVICE;
    const esp_err_t why = open_store(first);
    if (why == ESP_OK && cardfs_tainted()) {
        ESP_LOGW(TAG, "a card access failed during recovery, mounting again");
        return remount();
    }
    set_mounted(why);
    return ESP_OK;
}

// Nothing was written: the card is as it was, so mount it as boot does (a card that didn't mount then stays NO_CARD).
static void mount_untouched(esp_err_t why)
{
    esp_err_t err = find_partition(s_card);
    if (err == ESP_OK) {
        err = mount_fs();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "format: nothing was written, mounted %s again", SDCARD_MOUNT_POINT);
        attach_store();
        return;
    }
    ESP_LOGE(TAG, "format failed (%s) and the card didn't mount again: %s", esp_err_to_name(why), esp_err_to_name(err));
    s_error = err;
    set_state(SDCARD_NO_CARD, "SD: not mounted (%s)", esp_err_to_name(err));
}

esp_err_t sdcard_format(sdcard_format_reached_t *reached, const char **step)
{
    assert(svc_on_task());
    *reached = SDCARD_FORMAT_REFUSED;
    *step = "starting";
    if (!s_card_ready) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_owner == SDCARD_SHARED) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t capacity = (uint32_t)s_card->csd.capacity;
    if (s_card->csd.sector_size != CARDMKFS_SECTOR || capacity < CARDMKFS_MIN_SECTORS) {
        return ESP_ERR_INVALID_SIZE;
    }
    *step = "allocating memory";
    if (!s_verify_buf) {
        s_verify_buf =
            heap_caps_aligned_alloc(64, VERIFY_SECTORS * CARDMKFS_SECTOR, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    uint32_t work_len = 0;
    uint8_t *work = s_verify_buf ? format_buffer(&work_len) : NULL;
    if (!work) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t start = esp_timer_get_time();
    cardmkfs_layout_t layout;
    cardmkfs_layout(capacity, &layout);
    ESP_LOGW(TAG, "format: %lu sectors, FAT32 with %lu-byte clusters at lba %lu, a %lu-byte buffer (%u internal free)",
             (unsigned long)capacity, (unsigned long)layout.cluster_bytes, (unsigned long)layout.part_lba,
             (unsigned long)work_len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (s_store_inited) {
        cardfs_detach(); // waits for a call in progress; every later one says ESP_ERR_INVALID_STATE
    }
    unmount_fs(); // no-op when the boot mount or a remount failed
    s_fs_name = NULL;
    set_state(s_owner, "SD: formatting");

    const uint64_t checked_before = s_verify_sectors;
    esp_err_t err = format_card(capacity, work, work_len, step, reached);
    free(work);
    const int64_t mkfs_ms = (esp_timer_get_time() - start) / 1000;
    if (*reached == SDCARD_FORMAT_UNTOUCHED) {
        diag_count(DIAG_CARD_ERROR, "format failed, nothing written");
        mount_untouched(err);
        return err;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "format: written in %lld ms (%llu sectors read back)", (long long)mkfs_ms,
                 (unsigned long long)(s_verify_sectors - checked_before));
        *step = "reading the new partition table";
        err = find_partition(s_card);
    }
    if (err == ESP_OK && s_part_first_lba != 0) {
        err = ESP_ERR_INVALID_STATE; // a GPT header survived the wipe
    }
    if (err == ESP_OK) {
        *step = "mounting the card";
        err = mount_fs();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed while %s: %s", *step, esp_err_to_name(err));
        diag_count(DIAG_CARD_ERROR, "format failed");
        s_error = err;
        set_state(SDCARD_NO_CARD, "SD: not mounted (%s)", esp_err_to_name(err));
        return err; // cardfs stays detached, as after a failed remount
    }
    ESP_LOGI(TAG, "format: mounted %s in %lld ms", SDCARD_MOUNT_POINT,
             (long long)(esp_timer_get_time() - start) / 1000);
    *step = NULL;
    return attach_store();
}
