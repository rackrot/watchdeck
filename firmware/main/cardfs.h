#pragma once

// The only writer to the TF card. Every path is relative to the root given to cardfs_init and passes one
// allowlist: `wifi.txt`, or `watchdeck/<name>` with <name> 1 to 63 of [a-z0-9._-], no leading '.', no "..", and
// not ending in ".new" (the suffix of the intent records). Nothing here ever renames: an interrupted FatFs
// f_rename leaves two names on one cluster chain.
//
// Small files (wifi.txt, watchdeck/sleep.json) are written as an intent record `watchdeck/<key>.new` (a 48-byte
// header with the length, a write counter, the payload's SHA-256 and a CRC32 of the header, then the payload),
// then in place, then the record is emptied. cardfs_recover, run after every mount, redoes a valid record and
// empties a torn one, so a cut leaves each small file old or new. A record is emptied, never deleted, so after the
// first write its directory entry exists and FatFs never grows a directory for it (a cut while FatFs grows one can
// leave stale bytes in it). Big files (images) are written once under a content-addressed name and trusted only
// after cardfs_verify. A file just created must be found by name, or the write fails.
//
// Every call takes one mutex, so a write in progress finishes before cardfs_set_writable(false) returns.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CARDFS_SMALL_MAX 4096 // largest payload of cardfs_write_small
#define CARDFS_NAME_MAX 63    // longest <name> in watchdeck/<name>
#define CARDFS_PIECE 32768    // writes and reads go in pieces this big, so ESP-Hosted's SDIO traffic interleaves
#define CARDFS_RECORD_HDR 48  // "WDI1", u32 length, u32 generation, SHA-256, u32 CRC32 of the first 44 bytes

// Sets the root (e.g. "/sdcard", at most 15 characters) and creates the mutex. Writes are refused until
// cardfs_set_writable(true).
esp_err_t cardfs_init(const char *root);

// Allows or refuses writes, removes and recovery. Waits for a call in progress to finish.
void cardfs_set_writable(bool writable);
bool cardfs_writable(void);

// The allowlist. Lowercase only: FAT is case-insensitive, so other casings are refused, not folded.
bool cardfs_name_ok(const char *rel);

// Replaces a small file through its intent record. rel is "wifi.txt" or "watchdeck/sleep.json".
// ESP_ERR_INVALID_ARG: rel isn't a small file. ESP_ERR_INVALID_SIZE: len > CARDFS_SMALL_MAX.
// ESP_ERR_INVALID_STATE: not writable. ESP_FAIL: an I/O error; rel is old, or new once recovery has run.
esp_err_t cardfs_write_small(const char *rel, const void *data, size_t len);

// Creates rel and writes it once, fsynced at the end. ESP_ERR_NOT_ALLOWED: rel exists (delete an invalid file
// first). ESP_FAIL: an I/O error; the partial file is deleted if the card still answers, and cardfs_verify
// detects it otherwise.
esp_err_t cardfs_write_new(const char *rel, const void *data, size_t len);

// Reads rel whole into buf. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_SIZE: larger than cap.
esp_err_t cardfs_read(const char *rel, void *buf, size_t cap, size_t *len);

// Checks that rel is len bytes long and that its SHA-256 starts with digest (digest_len 1 to 32).
// ESP_OK: valid. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_CRC: wrong size or hash. ESP_FAIL: unreadable.
esp_err_t cardfs_verify(const char *rel, size_t len, const uint8_t *digest, size_t digest_len);

// Deletes rel. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_STATE: not writable.
esp_err_t cardfs_remove(const char *rel);

// Calls cb for every entry of watchdeck/, including names the allowlist refuses (a file a person copied there).
// ESP_ERR_NOT_FOUND: no watchdeck/ directory.
typedef void (*cardfs_list_cb_t)(const char *name, size_t size, bool is_dir, void *ctx);
esp_err_t cardfs_list(cardfs_list_cb_t cb, void *ctx);

// Resolves every intent record: a valid one is applied (rel rewritten if it differs) and emptied, a torn one is
// emptied, an empty or absent one means no write was in progress. Idempotent, so a cut during recovery is recovered
// the same way at the next mount.
// ESP_ERR_INVALID_STATE: not writable. ESP_FAIL: an I/O error left a record in place.
esp_err_t cardfs_recover(void);
