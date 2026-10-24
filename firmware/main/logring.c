#include "logring.h"

#include <string.h>

typedef struct {
    uint32_t seq;
    uint32_t ms;
    uint8_t level;
    uint8_t len;
} rec_hdr_t;

static void ring_put(logring_t *r, size_t pos, const void *src, size_t n)
{
    const size_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(r->buf + pos, src, first);
    memcpy(r->buf, (const uint8_t *)src + first, n - first);
}

static void ring_get(const logring_t *r, size_t pos, void *dst, size_t n)
{
    const size_t first = r->cap - pos < n ? r->cap - pos : n;
    memcpy(dst, r->buf + pos, first);
    memcpy((uint8_t *)dst + first, r->buf, n - first);
}

static rec_hdr_t get_hdr(const logring_t *r, size_t pos)
{
    uint8_t raw[LOGRING_HEADER];
    ring_get(r, pos, raw, sizeof(raw));
    rec_hdr_t h;
    memcpy(&h.seq, raw, 4);
    memcpy(&h.ms, raw + 4, 4);
    h.level = raw[8];
    h.len = raw[9];
    return h;
}

static size_t advance(const logring_t *r, size_t pos, size_t n)
{
    return (pos + n) % r->cap;
}

bool logring_init(logring_t *r, uint8_t *buf, size_t cap)
{
    if (!r || !buf || cap < LOGRING_MIN_CAP) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    r->buf = buf;
    r->cap = cap;
    r->next_seq = 1;
    return true;
}

static void set_secret(char *dst, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n < LOGRING_SECRET_MIN) {
        n = 0;
    } else if (n > LOGRING_SECRET_MAX) {
        n = LOGRING_SECRET_MAX;
    }
    if (n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

void logring_set_token(logring_t *r, const char *token_hex)
{
    set_secret(r->token, token_hex);
}

void logring_set_password(logring_t *r, const char *pass)
{
    set_secret(r->pass, pass);
}

/* ---- Redaction ---- */

#define MARK_LEN (sizeof(LOGRING_REDACTED) - 1)

// Replace text[at..at+n) with the marker, keeping the text under cap - 1 bytes. Returns the new length.
static size_t put_marker(char *text, size_t len, size_t cap, size_t at, size_t n)
{
    const size_t limit = cap - 1;
    const size_t tail = len - at - n;
    size_t marker = MARK_LEN;
    size_t tail_kept = tail;
    if (at + MARK_LEN > limit) {
        marker = limit - at;
        tail_kept = 0;
    } else if (at + MARK_LEN + tail > limit) {
        tail_kept = limit - at - MARK_LEN;
    }
    memmove(text + at + MARK_LEN, text + at + n, tail_kept); // before the marker, which overlaps the old tail
    memcpy(text + at, LOGRING_REDACTED, marker);
    const size_t out = at + marker + tail_kept;
    text[out] = '\0';
    return out;
}

static size_t redact_whole(char *text, size_t len, size_t cap, const char *s)
{
    const size_t sl = strlen(s);
    for (size_t i = 0; i + sl <= len;) {
        if (text[i] == s[0] && memcmp(text + i, s, sl) == 0) {
            len = put_marker(text, len, cap, i, sl);
            i += MARK_LEN;
        } else {
            i++;
        }
    }
    return len;
}

// The longest tail of text that is a proper prefix of s, at least LOGRING_SECRET_MIN long.
static size_t redact_tail(char *text, size_t len, size_t cap, const char *s)
{
    const size_t sl = strlen(s);
    for (size_t k = sl - 1 < len ? sl - 1 : len; k >= LOGRING_SECRET_MIN; k--) {
        if (memcmp(text + len - k, s, k) == 0) {
            return put_marker(text, len, cap, len - k, k);
        }
    }
    return len;
}

size_t logring_redact(char *text, size_t len, size_t cap, bool truncated, const char *a, const char *b)
{
    const char *secrets[2] = {a, b};
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 2; i++) {
            const char *s = secrets[i];
            if (!s || strlen(s) < LOGRING_SECRET_MIN) {
                continue;
            }
            len = pass == 0 ? redact_whole(text, len, cap, s) : truncated ? redact_tail(text, len, cap, s) : len;
        }
    }
    return len;
}

/* ---- Storing ---- */

// In place: drop ANSI colour sequences and the trailing line end, make the rest printable ASCII, cut. Returns the
// length.
static size_t clean(char *text, size_t len)
{
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    size_t out = 0;
    for (size_t i = 0; i < len && out < LOGRING_TEXT_MAX; i++) {
        const unsigned char c = (unsigned char)text[i];
        if (c == 0x1b) {
            if (i + 1 < len && text[i + 1] == '[') {
                i += 2;
                while (i < len && !((unsigned char)text[i] >= 0x40 && (unsigned char)text[i] <= 0x7e)) {
                    i++;
                }
            }
            continue;
        }
        text[out++] = (c >= 0x20 && c <= 0x7e) ? (char)c : '?';
    }
    text[out] = '\0';
    return out;
}

static char level_of(const char *text, size_t len)
{
    if (len >= 3 && (text[0] == 'E' || text[0] == 'W' || text[0] == 'I') && text[1] == ' ' && text[2] == '(') {
        return text[0];
    }
    return 'I';
}

static void drop_oldest(logring_t *r)
{
    const rec_hdr_t h = get_hdr(r, r->head);
    const size_t n = LOGRING_HEADER + h.len;
    r->head = advance(r, r->head, n);
    r->used -= n;
    r->count--;
}

void logring_append(logring_t *r, char *text, size_t len, uint32_t ms)
{
    len = clean(text, len);
    const size_t need = LOGRING_HEADER + len;
    while (r->used + need > r->cap) {
        drop_oldest(r);
    }
    uint8_t raw[LOGRING_HEADER];
    memcpy(raw, &r->next_seq, 4);
    memcpy(raw + 4, &ms, 4);
    raw[8] = (uint8_t)level_of(text, len);
    raw[9] = (uint8_t)len;
    const size_t tail = advance(r, r->head, r->used);
    ring_put(r, tail, raw, sizeof(raw));
    ring_put(r, advance(r, tail, sizeof(raw)), text, len);
    r->used += need;
    r->count++;
    r->next_seq++;
}

bool logring_capture(logring_t *r, char *text, size_t cap, int ret, uint32_t ms)
{
    if (ret < 0) {
        return false;
    }
    const bool truncated = (size_t)ret >= cap;
    const size_t len = truncated ? cap - 1 : (size_t)ret;
    const size_t out = logring_redact(text, len, cap, truncated, r->token, r->pass);
    logring_append(r, text, out, ms);
    return true;
}

/* ---- Reading ---- */

uint32_t logring_next_seq(const logring_t *r)
{
    return r->next_seq;
}

uint32_t logring_count(const logring_t *r)
{
    return r->count;
}

static bool wanted(const rec_hdr_t *h, bool errors_only)
{
    return !errors_only || h->level == 'E' || h->level == 'W';
}

static void copy_out(const logring_t *r, size_t pos, const rec_hdr_t *h, logring_line_t *line)
{
    line->seq = h->seq;
    line->ms = h->ms;
    line->level = (char)h->level;
    line->len = h->len;
    ring_get(r, advance(r, pos, LOGRING_HEADER), line->text, h->len);
    line->text[h->len] = '\0';
}

size_t logring_read_from(const logring_t *r, uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max)
{
    size_t n = 0;
    size_t pos = r->head;
    for (uint32_t i = 0; i < r->count && n < max; i++) {
        const rec_hdr_t h = get_hdr(r, pos);
        if (h.seq >= from_seq && wanted(&h, errors_only)) {
            copy_out(r, pos, &h, &out[n++]);
        }
        pos = advance(r, pos, LOGRING_HEADER + h.len);
    }
    return n;
}

size_t logring_read_before(const logring_t *r, uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max)
{
    size_t matches = 0;
    size_t pos = r->head;
    for (uint32_t i = 0; i < r->count; i++) {
        const rec_hdr_t h = get_hdr(r, pos);
        if (h.seq < before_seq && wanted(&h, errors_only)) {
            matches++;
        }
        pos = advance(r, pos, LOGRING_HEADER + h.len);
    }
    size_t skip = matches > max ? matches - max : 0;
    size_t n = 0;
    pos = r->head;
    for (uint32_t i = 0; i < r->count && n < max; i++) {
        const rec_hdr_t h = get_hdr(r, pos);
        if (h.seq < before_seq && wanted(&h, errors_only)) {
            if (skip) {
                skip--;
            } else {
                copy_out(r, pos, &h, &out[n++]);
            }
        }
        pos = advance(r, pos, LOGRING_HEADER + h.len);
    }
    return n;
}
