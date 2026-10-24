#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A byte ring of log lines for the Logs tab. Pure: no ESP-IDF, no locking, no clock (the caller passes the time),
 * so the host tests compile it. logcap.c owns the lock and the vprintf hook around it.
 *
 * Each record is a 10-byte header (seq u32, ms u32, level u8, len u8) and the text. When a new record doesn't fit,
 * the oldest records go, whole. Sequence numbers only grow, so a reader can page by them while lines arrive. A
 * checkpoint index (the offset of every 16th sequence number) lets a read start near its sequence number instead of
 * walking the ring from the oldest record.
 *
 * Every line is made printable ASCII before it is stored: ANSI colour codes go, a trailing CR/LF goes, and any other
 * byte outside 0x20 to 0x7e becomes '?', since the device's fonts have nothing else. Lines are cut at
 * LOGRING_TEXT_MAX bytes.
 *
 * Secrets: logring_capture cleans the formatted text, then redacts the paired token (64 hex digits) and the Wi-Fi
 * passwords from it, and only then cuts it, so a secret is stored whole as "<redacted>", or not at all. Only a
 * secret of LOGRING_WHOLE_MIN (8, the WPA minimum) characters or more is matched whole; a shorter "password" would
 * wipe ordinary words from every line. A cut-off prefix of a secret is matched from LOGRING_PREFIX_MIN characters.
 */

#define LOGRING_TEXT_MAX 159 // bytes of a stored line
#define LOGRING_FMT_BUF 224  // the formatted text, NUL included, that logring_capture takes
#define LOGRING_HEADER 10    // bytes in front of each record's text
#define LOGRING_MIN_CAP (LOGRING_HEADER + LOGRING_TEXT_MAX)
#define LOGRING_MAX_CAP 65536 // the checkpoint index is sized for this
#define LOGRING_WHOLE_MIN 8   // shortest secret matched whole
#define LOGRING_PREFIX_MIN 4  // shortest cut-off prefix of a secret that is matched
#define LOGRING_SECRET_MAX 64 // bytes kept of a password; the token is 64 hex digits
#define LOGRING_PASS_SLOTS 2  // the current Wi-Fi password and a candidate (a trial connect)
#define LOGRING_REDACTED "<redacted>"
#define LOGRING_STEP 16 // checkpoint spacing, in sequence numbers
#define LOGRING_IDX_N 512

typedef struct {
    uint32_t seq;
    uint32_t ms; // since boot, as passed to logring_append
    char level;  // 'E', 'W' or 'I'
    uint8_t len; // strlen(text)
    char text[LOGRING_TEXT_MAX + 1];
} logring_line_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;                // offset of the oldest record
    size_t used;                // bytes in records
    uint32_t next_seq;          // the sequence number the next line gets (the first is 1, so 0 is never a line)
    uint32_t count;             // lines held
    uint32_t cp[LOGRING_IDX_N]; // offset of the record whose seq is a multiple of LOGRING_STEP, at [seq / STEP % N]
    char token[LOGRING_SECRET_MAX + 1];
    char pass[LOGRING_PASS_SLOTS][LOGRING_SECRET_MAX + 1];
} logring_t;

// `buf` of `cap` bytes (LOGRING_MIN_CAP to LOGRING_MAX_CAP) is the ring's storage. Returns false on a bad argument.
bool logring_init(logring_t *r, uint8_t *buf, size_t cap);

// The secrets logring_capture redacts. NULL or "" clears one. A password shorter than LOGRING_WHOLE_MIN is not kept
// (an open network has none), and one longer than LOGRING_SECRET_MAX is cut to that. `slot` is 0 (current) or 1
// (candidate); an out-of-range slot is ignored.
void logring_set_token(logring_t *r, const char *token_hex);
void logring_set_password(logring_t *r, int slot, const char *pass);

/*
 * Replace every whole occurrence of each of `secrets[0..n)` in text[0..len) with "<redacted>". When `*truncated`
 * (the formatter ran out of room), also replace a tail that is a prefix of one, LOGRING_PREFIX_MIN characters or
 * more, because the rest of that secret was cut off. A marker that is longer than the secret can push the text past
 * cap - 1: the end is cut and `*truncated` becomes true, so the tail check covers that cut too. Secrets shorter than
 * LOGRING_WHOLE_MIN and NULL entries are skipped. `cap` is the size of the buffer, NUL included. Returns the new
 * length and NUL-terminates.
 */
size_t logring_redact(char *text, size_t len, size_t cap, bool *truncated, const char *const secrets[], size_t n);

/*
 * Store one line: cleans `text` in place (it is the caller's scratch), cuts it to LOGRING_TEXT_MAX and appends it.
 * `len` is the number of bytes of text. The level is 'E' or 'W' or 'I' when the text starts "X (", else 'I'.
 */
void logring_append(logring_t *r, char *text, size_t len, uint32_t ms);

/*
 * The hook's whole job after vsnprintf: `text` is a buffer of `cap` bytes that vsnprintf(text, cap, ...) filled and
 * `ret` is its return value. Cleans, redacts the token and the passwords, cuts and appends. Returns false, storing
 * nothing, when ret is negative.
 */
bool logring_capture(logring_t *r, char *text, size_t cap, int ret, uint32_t ms);

// The sequence number the next line will get, the oldest held (equal to next_seq when empty), and the lines held.
uint32_t logring_next_seq(const logring_t *r);
uint32_t logring_oldest_seq(const logring_t *r);
uint32_t logring_count(const logring_t *r);

/*
 * Copy up to `max` lines, oldest first, into `out`. `from`: the lines with seq >= from_seq (the next page forward).
 * `before`: the last `max` lines with seq < before_seq (the page that ends where another began; pass next_seq for
 * the newest page). `errors_only` keeps levels E and W. Returns the number copied. A later append never changes the
 * lines a given seq names, so a page read again after more lines arrive is the same.
 *
 * Without `errors_only` a read touches at most LOGRING_STEP + max records. With it, a read may walk every record
 * when few lines are E or W.
 */
size_t logring_read_from(const logring_t *r, uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max);
size_t logring_read_before(const logring_t *r, uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max);
