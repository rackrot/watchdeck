#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SDCARD_MOUNT_POINT "/sdcard"

// Who has the TF card. The firmware from boot (DEVICE); USB once a share has mapped it (SHARED), until the next
// reboot, since taking it back at run time races esp_tinyusb's callbacks; or nobody (NO_CARD: absent, unreadable, or
// a remount failed). Only the firmware's cardfs.c writes to the card, and only while DEVICE.
typedef enum {
    SDCARD_NO_CARD,
    SDCARD_DEVICE,
    SDCARD_SHARED,
} sdcard_owner_t;

// Called after each change of the owner or the SD status text, from the task that made it (svc after boot), never
// with cardfs's mutex held.
typedef void (*sdcard_owner_cb_t)(sdcard_owner_t owner, const char *sd_text);

/**
 * At boot, before anything reads the card: init the card, mount its first GPT partition read-write (never formats),
 * then cardfs_init, cardfs_set_writable(true) and cardfs_recover. A recovery that fails is logged, counted and left
 * for the next mount; one that taints cardfs gets one remount at once. A card cardfs can't write (not FAT32) stays
 * mounted read-only. Writes "SD: <size>", "SD: <size>, read-only (<why>)" or "SD: not mounted (<err>)" into msg.
 * Returns the mount's error (NO_CARD) or ESP_OK.
 */
esp_err_t sdcard_mount(char *msg, size_t msg_len);

void sdcard_set_owner_cb(sdcard_owner_cb_t cb);
sdcard_owner_t sdcard_owner(void);

/** The initialised card, valid after sdcard_mount() found one (also while shared). */
sdmmc_card_t *sdcard_card(void);

// The calls below run on the svc task only, like every card access after boot (svc.h), so none of them races a
// cardfs call or another of them.

/**
 * Unmount and mount the card again, then cardfs_set_writable(true) and cardfs_recover. What cardfs needs after a
 * card access failed (cardfs_tainted()): FatFs may hold a sector it couldn't write, and only a fresh mount drops it.
 * ESP_ERR_INVALID_STATE: the card isn't the device's. A failed mount leaves NO_CARD.
 */
esp_err_t sdcard_remount(void);

/**
 * After a cardfs call failed with err: logs it with what, counts it (DIAG_CARD_ERROR), and remounts when cardfs is
 * tainted, so the next write can go ahead. Every card writer calls it on a failure.
 */
void sdcard_note_failure(const char *what, esp_err_t err);

/**
 * For the USB share: refuse cardfs writes (waits for a call in progress), unmount and unregister the file system.
 * The card and the SDMMC host stay initialised (the host is shared with ESP-Hosted). The owner stays DEVICE until
 * sdcard_set_shared; if the share fails, sdcard_remount takes the card back. ESP_ERR_INVALID_STATE: not DEVICE.
 */
esp_err_t sdcard_release_fs(void);

/** The card is mapped to USB: SHARED until the next reboot. */
void sdcard_set_shared(void);
