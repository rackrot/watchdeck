#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * A byte ring of log lines for the Logs tab. Pure: no ESP-IDF, no locking, no clock (the caller passes the time),
 * so the host tests compile it. logcap.c owns the lock and the vprintf hook around it.
 *
 * Each record is a 10-byte header (seq u32, ms u32, level u8, len u8) and the text. When a new record doesn't fit,
 * the oldest records go, whole. Sequence numbers only grow, so a reader can page by them while lines arrive.
 *
 * Every line is made printable ASCII before it is stored: ANSI colour codes go, a trailing CR/LF goes, and any other
 * byte outside 0x20 to 0x7e becomes '?', since the device's fonts have nothing else. Lines are cut at
 * LOGRING_TEXT_MAX bytes.
 *
 * Secrets: logring_capture redacts the paired token (64 hex digits) and the Wi-Fi password from the formatted text
 * before the cut, so a secret is stored whole as "<redacted>", or not at all (see logring_redact).
 */

#define LOGRING_TEXT_MAX 159 // bytes of a stored line
#define LOGRING_FMT_BUF 224  // the formatted text, NUL included, that logring_capture takes
#define LOGRING_HEADER 10    // bytes in front of each record's text
#define LOGRING_MIN_CAP (LOGRING_HEADER + LOGRING_TEXT_MAX)
#define LOGRING_SECRET_MIN 4  // a shorter password is left alone, and so is a shorter cut-off prefix
#define LOGRING_SECRET_MAX 64 // bytes kept of a password; the token is 64 hex digits
#define LOGRING_REDACTED "<redacted>"

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
    size_t head;       // offset of the oldest record
    size_t used;       // bytes in records
    uint32_t next_seq; // the sequence number the next line gets (the first is 1, so 0 is never a line)
    uint32_t count;    // lines held
    char token[LOGRING_SECRET_MAX + 1];
    char pass[LOGRING_SECRET_MAX + 1];
} logring_t;

// `buf` of `cap` bytes (at least LOGRING_MIN_CAP) is the ring's storage. Returns false on a bad argument.
bool logring_init(logring_t *r, uint8_t *buf, size_t cap);

// The secrets logring_capture redacts. NULL or "" clears one. A password shorter than LOGRING_SECRET_MIN is not kept,
// and one longer than LOGRING_SECRET_MAX is cut to that.
void logring_set_token(logring_t *r, const char *token_hex);
void logring_set_password(logring_t *r, const char *pass);

/*
 * Replace every whole occurrence of `a` and `b` in text[0..len) with "<redacted>". When `truncated` (the formatter ran
 * out of room), also replace a tail that is a prefix of either, LOGRING_SECRET_MIN characters or more, because the
 * rest of that secret was cut off. A secret shorter than LOGRING_SECRET_MIN is ignored. `cap` is the size of the
 * buffer, NUL included; the text never grows past cap - 1. NULL secrets are skipped. Returns the new length and
 * NUL-terminates.
 */
size_t logring_redact(char *text, size_t len, size_t cap, bool truncated, const char *a, const char *b);

/*
 * Store one line: strips, replaces and cuts `text` in place (it is the caller's scratch), then appends it. `len` is
 * the number of bytes of text. The level is 'E' or 'W' or 'I' when the text starts "X (", else 'I'.
 */
void logring_append(logring_t *r, char *text, size_t len, uint32_t ms);

/*
 * The hook's whole job after vsnprintf: `text` is a buffer of `cap` bytes that vsnprintf(text, cap, ...) filled and
 * `ret` is its return value. Redacts the token and the password (including a cut-off prefix when ret >= cap), then
 * appends. Returns false, storing nothing, when ret is negative.
 */
bool logring_capture(logring_t *r, char *text, size_t cap, int ret, uint32_t ms);

// The sequence number the next line will get, and how many lines the ring holds. The oldest held is next_seq - count.
uint32_t logring_next_seq(const logring_t *r);
uint32_t logring_count(const logring_t *r);

/*
 * Copy up to `max` lines, oldest first, into `out`. `from`: the lines with seq >= from_seq (the next page forward).
 * `before`: the last `max` lines with seq < before_seq (the page that ends where another began; pass next_seq for
 * the newest page). `errors_only` keeps levels E and W. Returns the number copied. A later append never changes the
 * lines a given seq names, so a page read again after more lines arrive is the same.
 */
size_t logring_read_from(const logring_t *r, uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max);
size_t logring_read_before(const logring_t *r, uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max);
