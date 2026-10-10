#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "sdmmc_cmd.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/**
 * Mount the TF card read-only (never formats or writes, never unmounts: the
 * SDMMC host is shared with Wi-Fi). Writes "SD: <size>" or "SD: not mounted
 * (<err>)" into msg for display.
 */
esp_err_t sdcard_mount(char *msg, size_t msg_len);

/** The initialised card, valid after a successful sdcard_mount() (and after sdcard_release_fs()). */
sdmmc_card_t *sdcard_card(void);

/**
 * Drop the firmware's FatFs mount and VFS registration so /sdcard stops working and the
 * raw card can go to a USB host. The card and SDMMC host stay initialised (the host is
 * shared with ESP-Hosted). No-op if nothing is mounted.
 */
void sdcard_release_fs(void);
