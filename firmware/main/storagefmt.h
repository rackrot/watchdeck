#pragma once

/*
 * The Storage tab's row texts, pure so the host tests cover them. Names that come from the card (the SSID, file names)
 * are cut to printable ASCII here, and no text is ever recolor markup because the labels have recolor off.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wifitxt.h"

#define STORAGEFMT_SIZE_MAX 16 // "1023.9 KB" and a NUL, with room
#define STORAGEFMT_IMG_NAME 64 // the longest image name kept (CARDFS_NAME_MAX + 1)

/** "812 B", "1.5 KB", "59.5 GB": 1024-based like the SD status line, one decimal, rounded down. */
void storagefmt_size(char out[STORAGEFMT_SIZE_MAX], uint64_t bytes);

/** Whether a card read worked, and why not when it didn't. */
typedef enum {
    STORAGEFMT_NOT_READ, // no read has finished yet
    STORAGEFMT_READ,
    STORAGEFMT_SHARED, // the card is shared over USB, so nothing is known
    STORAGEFMT_NO_CARD,
    STORAGEFMT_ERROR,
} storagefmt_access_t;

typedef enum {
    STORAGEFMT_OWNER_NONE,
    STORAGEFMT_OWNER_DEVICE,
    STORAGEFMT_OWNER_SHARED,
} storagefmt_owner_t;

/** "Card: SD64G, 59.5 GB, FAT32" (name, size and file system as far as known), plus ", read-only"; "Card: none". */
void storagefmt_card(char *out, size_t n, bool present, const char *name, uint64_t bytes, const char *fs,
                     bool read_only);

/** "Owner: on the device", "shared over USB", "shared, no computer attached", "no card" or "unreadable: <error>". */
void storagefmt_owner(char *out, size_t n, storagefmt_owner_t owner, bool present, bool connected, const char *error);

typedef enum {
    STORAGEFMT_WIFI_ABSENT,
    STORAGEFMT_WIFI_FOUND,   // with a network
    STORAGEFMT_WIFI_BLANK,   // line 1 is empty: the board connects to nothing
    STORAGEFMT_WIFI_INVALID, // it can't be used: empty, or a line too long
} storagefmt_wifi_t;

/**
 * Read the state of a wifi.txt that was found, from its bytes, with wifitxt_parse. The SSID goes to `ssid`; the
 * password is parsed into a buffer that is wiped before this returns, and nothing here copies it anywhere else.
 */
storagefmt_wifi_t storagefmt_wifi_parse(const void *data, size_t len, char ssid[WIFITXT_SSID_MAX + 1]);

/** "wifi.txt: found, network <ssid>", "...: not found", "...: unknown (card shared)", "...: no card". */
void storagefmt_wifi(char *out, size_t n, storagefmt_access_t access, storagefmt_wifi_t file, const char *ssid);

/**
 * "Sleep files: sleep.cfg, <name>.img 1.0 MB", "none", "sleep.cfg, no image", "no sleep.cfg, <name>.img 1.0 MB", and
 * "and N more" past the first image. `img_name` is the first image's.
 */
void storagefmt_sleep(char *out, size_t n, storagefmt_access_t access, bool cfg, size_t images, const char *img_name,
                      uint64_t img_size);

/** "Free space: not checked", "12.3 GB of 59.5 GB". */
void storagefmt_free(char *out, size_t n, bool known, uint64_t free_bytes, uint64_t total);
