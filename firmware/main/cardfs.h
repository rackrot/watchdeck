#pragma once

// The only writer to the TF card. Every path is relative to the root given to cardfs_init and passes one
// allowlist: `wifi.txt`, or `watchdeck/<name>` with <name> 1 to 63 of [a-z0-9._-], no leading '.', no "..", and
// not ending in ".new" (the suffix of the intent records). A lowercase 8.3 name (one dot at most, a body of 1 to 8
// and an extension of up to 3) takes one directory entry; any other takes one more per 13 characters, so callers
// pick 8.3 names. Nothing here ever renames or truncates: an interrupted FatFs f_rename leaves two names on one
// cluster chain, and f_truncate frees a chain before it updates the entry.
//
// Small files (wifi.txt, watchdeck/sleep.cfg) are written as an intent record `watchdeck/<key>.new` (a 48-byte
// header with the length, a write counter, the payload's SHA-256 and a CRC32 of the header, then the payload),
// then in place, then the record is emptied. cardfs_recover, run after every mount, redoes a valid record and
// empties a torn one, so a cut leaves each small file old or new. A record that can't be checked (a read or hash
// failure) is left as it is. A record is emptied, never deleted, so after the first write its directory entry
// exists. Big files (images) are written once under their own name and trusted only after cardfs_verify.
//
// No call ever makes FatFs grow a directory or split a name across sectors. FatFs links a directory's new cluster
// before it zeroes it (ff.c dir_next, then dir_clear), so a cut between the two leaves stale bytes in the
// directory: FatFs can then place a later entry past them, where no lookup finds it, and a PC sees them as junk
// entries. And FatFs writes a name's long-name entries and its 8.3 entry sector by sector, so a cut between two
// sectors leaves orphan long-name entries, which fsck reports. So before a call adds an entry it asks the volume's
// room function whether the directory's allocated clusters can take it, each name inside one sector, and refuses
// with CARDFS_ERR_DIR_FULL when they can't. A file just created must also be found by name, or the write fails.
//
// Any failed card access (other than "absent" or "exists") taints cardfs: FatFs may still hold a sector it couldn't
// write, which the room function, reading the disk raw, can't see. While tainted, every write, remove, recovery and
// room check returns ESP_ERR_INVALID_STATE, until the caller remounts the card and then calls cardfs_init, or
// cardfs_set_writable(false) and then (true). Reads go on.
//
// A recovery redoes a valid record whatever wifi.txt holds by then: a person who edits wifi.txt on a PC after a cut,
// before the board mounts the card again, has that edit replaced by the write the cut interrupted.
//
// Boot order, after the card is mounted read-write and before anything else touches it: cardfs_init, then
// cardfs_set_writable(true), then cardfs_recover. Before USB takes the card: cardfs_set_writable(false), which waits
// for a call in progress. Every call takes one mutex.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define CARDFS_SMALL_MAX 4096 // largest payload of cardfs_write_small
#define CARDFS_NAME_MAX 63    // longest <name> in watchdeck/<name>
#define CARDFS_PIECE 32768    // writes and reads go in pieces this big, so ESP-Hosted's SDIO traffic interleaves
#define CARDFS_RECORD_HDR 48  // "WDI1", u32 length, u32 generation, SHA-256, u32 CRC32 of the first 44 bytes

// The card's root directory or watchdeck/ has no run of free entries long enough for a new name in the clusters it
// has, so FatFs would have to grow it; or the run FatFs would use for a long name crosses a sector boundary; or the
// directory holds entries past its end marker (only a cut while FatFs grew it leaves those). Nothing was written.
// Deleting or adding a few files in that directory on a PC fixes the first two.
#define CARDFS_ERR_DIR_FULL 0x7D01

// The volume cardfs writes to.
typedef struct {
    // Checks the mounted volume and readies it for writes. Run by cardfs_init and by cardfs_set_writable(true), so
    // after every mount. NULL: nothing to do.
    esp_err_t (*prepare)(void *ctx);
    // Checks whether dir ("" for the root, or "watchdeck") can take n new entry runs, runs[i] entries each,
    // allocated in that order the way FatFs's dir_alloc does, without FatFs growing it and with each run inside one
    // sector. ESP_OK: it can. CARDFS_ERR_DIR_FULL: it can't (see above). ESP_FAIL: a read error or a damaged
    // directory.
    esp_err_t (*room)(const char *dir, const uint8_t *runs, size_t n, void *ctx);
    void *ctx;
} cardfs_volume_t;

// The FatFs volume (cardfs_fat.c); ctx is the FatFs drive, e.g. "0:", as sdcard.c registered it.
// cardfs_fat_prepare refuses anything but FAT32 (ESP_ERR_NOT_SUPPORTED) and sets fs->free_clst and fs->last_clst
// to 0xFFFFFFFF: IDF 6.1 always trusts FSINFO (ffconf.h FF_FS_NOFSINFO 0, whatever the CONFIG_FATFS_DONT_TRUST_*
// options say), and a cut can leave its free count stale, which makes FatFs report a full card falsely.
// cardfs_fat_room reads the directory's entries and FAT chain straight from the disk and replays dir_alloc over
// them, so it's exact while FatFs has nothing unsynced: true between cardfs calls on an untainted mount.
esp_err_t cardfs_fat_prepare(void *ctx);
esp_err_t cardfs_fat_room(const char *dir, const uint8_t *runs, size_t n, void *ctx);

// Sets the root (e.g. "/sdcard", at most 15 characters) and the volume (copied; vol->ctx is kept), creates the
// mutex, clears the taint and runs vol->prepare, returning its error. Writes are refused until
// cardfs_set_writable(true).
esp_err_t cardfs_init(const char *root, const cardfs_volume_t *vol);

// Allows or refuses writes, removes and recovery. Waits for a call in progress to finish. false also clears the
// taint. true runs vol->prepare first and stays unwritable if it fails, returning its error.
esp_err_t cardfs_set_writable(bool writable);
bool cardfs_writable(void);

// The allowlist. Lowercase only: FAT is case-insensitive, so other casings are refused, not folded.
bool cardfs_name_ok(const char *rel);

// Replaces a small file through its intent record. rel is "wifi.txt" or "watchdeck/sleep.cfg". First resolves a
// record an earlier failed write left, and refuses if that fails, so a pending write is never lost to a new one.
// ESP_ERR_INVALID_ARG: rel isn't a small file. ESP_ERR_INVALID_SIZE: len > CARDFS_SMALL_MAX.
// ESP_ERR_INVALID_STATE: not writable, or tainted. CARDFS_ERR_DIR_FULL: see above; nothing written.
// ESP_ERR_NO_MEM: nothing written. ESP_FAIL: an I/O error; rel is as an earlier write left it, or new once
// recovery has run. ESP_OK also when the in-place write failed but its record, tried once more at once, wrote it
// and rel then reads back as data (cardfs is tainted all the same).
esp_err_t cardfs_write_small(const char *rel, const void *data, size_t len);

// Creates rel and writes it once, fsynced at the end. ESP_ERR_NOT_ALLOWED: rel exists (delete an invalid file
// first). CARDFS_ERR_DIR_FULL: see above; nothing written. ESP_ERR_INVALID_STATE: not writable, or tainted.
// ESP_FAIL: an I/O error; the partial file is deleted if the card still answers, and cardfs_verify detects it
// otherwise.
esp_err_t cardfs_write_new(const char *rel, const void *data, size_t len);

// Reads rel whole into buf. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_SIZE: larger than cap.
esp_err_t cardfs_read(const char *rel, void *buf, size_t cap, size_t *len);

// Checks that rel is len bytes long and that its SHA-256 starts with digest (digest_len 1 to 32).
// ESP_OK: valid. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_CRC: wrong size or hash. ESP_FAIL: unreadable.
esp_err_t cardfs_verify(const char *rel, size_t len, const uint8_t *digest, size_t digest_len);

// Deletes rel. ESP_ERR_NOT_FOUND: absent. ESP_ERR_INVALID_STATE: not writable, or tainted.
esp_err_t cardfs_remove(const char *rel);

// Calls cb for every entry of watchdeck/, including names the allowlist refuses (a file a person copied there) and
// the empty intent records. The entries are read first and cb runs after the mutex is released, so cb may call
// cardfs. ESP_ERR_NOT_FOUND: no watchdeck/ directory. ESP_ERR_NO_MEM, or ESP_FAIL on a read or stat error: cb is
// never called.
typedef void (*cardfs_list_cb_t)(const char *name, size_t size, bool is_dir, void *ctx);
esp_err_t cardfs_list(cardfs_list_cb_t cb, void *ctx);

// Resolves every intent record: a valid one is applied (rel rewritten if it differs) and emptied, a torn one is
// emptied, an empty or absent one means no write was in progress. Idempotent, so a cut during recovery is recovered
// the same way at the next mount.
// ESP_ERR_INVALID_STATE: not writable, or tainted. ESP_FAIL: an I/O or hash error, or no room to recreate a missing
// file, left a record in place for the next mount.
esp_err_t cardfs_recover(void);

// For tests: the SHA-256 computation cardfs makes after `after` more succeed fails once, as a crypto driver error
// would (cardfs_verify's streaming hash excepted). -1 turns it off.
void cardfs_test_fail_sha256(int after);
