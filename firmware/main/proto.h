#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "font_ids.h"

/* Wire format: site/src/protocol.md. Every function here is pure (no I/O, no globals). */

#define PROTO_VERSION 1
#define PROTO_MAX_MSG 16384 // one text frame, either direction
#define PROTO_MAX_COLS 8
#define PROTO_MAX_ROWS 5
#define PROTO_MAX_BUTTONS (PROTO_MAX_COLS * PROTO_MAX_ROWS)
#define PROTO_MAX_PAGES 4
#define PROTO_ID_MAX 64
#define PROTO_LABEL_MAX 48
#define PROTO_NAME_MAX 48
#define PROTO_TEXT_MAX 1025      // up to 1 KB of output plus NUL
#define PROTO_SUMMARY_MAX 97     // a result's one-line summary, 96 characters plus NUL
#define PROTO_TILE_MAX 65        // a tile's text, 64 characters plus NUL
#define PROTO_DEVICE_ID_LEN 16   // "wd-" + 12 hex + NUL
#define PROTO_FP_LEN 16          // hex digits of the daemon cert fingerprint in mDNS TXT "fp"
#define PROTO_TOKEN_LEN 32       // bytes in the device token
#define PROTO_HASH_LEN 32        // bytes in a SHA-256 digest (commit, c_host)
#define PROTO_NONCE_LEN 16       // bytes in each pairing nonce (n_dev, n_host)
#define PROTO_CERT_MAX 2048      // the daemon cert PEM plus NUL
#define PROTO_REPORT_ID_LEN 9    // 8 hex digits plus NUL
#define PROTO_ICON_ID_LEN 16     // bytes in an icon id, shown as 32 hex digits in a layout
#define PROTO_ICON_HEADER_LEN 24 // "WDI1", the id, then width and height as u16 LE
#define PROTO_ICON_MAX_SIDE 256
#define PROTO_MAX_ICON_FRAME (PROTO_ICON_HEADER_LEN + PROTO_ICON_MAX_SIDE * PROTO_ICON_MAX_SIDE * 4) // one binary frame

typedef enum {
    PROTO_MSG_UNKNOWN, // well-formed JSON with a "t" this firmware does not know
    PROTO_MSG_WELCOME,
    PROTO_MSG_ERROR,
    PROTO_MSG_LAYOUT,
    PROTO_MSG_RESULT,
    PROTO_MSG_TILE,
    PROTO_MSG_PAIR_OFFER,
    PROTO_MSG_PAIR_REVEAL,
    PROTO_MSG_PAIR_RESULT,
    PROTO_MSG_REPORT_ACK,
} proto_type_t;

typedef struct {
    char id[PROTO_ID_MAX];
    char label[PROTO_LABEL_MAX];
    uint8_t col;
    uint8_t row;
    uint32_t bg; // 0xRRGGBB
    uint32_t fg;
    bool has_icon;
    uint8_t icon[PROTO_ICON_ID_LEN]; // valid when has_icon
    bool tile;                       // shows a live value, sent as tile messages
    wd_font_t label_font;            // already resolved: label_font, else the message font
    wd_font_t value_font;            // already resolved: value_font, else label_font
} proto_button_t;

typedef struct {
    uint32_t rev;
    uint8_t page;               // 0-based, below pages. A layout without page fields is page 0 of 1
    uint8_t pages;              // 1 to PROTO_MAX_PAGES
    char page_id[PROTO_ID_MAX]; // empty for a layout without page fields
    uint8_t cols;
    uint8_t rows;
    wd_font_t font;    // the grid's default font
    bool unknown_font; // a font field was not a known name and was drawn as Montserrat
    size_t n_buttons;
    proto_button_t buttons[PROTO_MAX_BUTTONS];
} proto_layout_t;

typedef struct {
    char btn[PROTO_ID_MAX];
    uint32_t seq;
    bool ok;
    int code;
    char summary[PROTO_SUMMARY_MAX]; // "exit 0", "timed out after 10 s"; empty if the daemon sent none
    char text[PROTO_TEXT_MAX];
    uint32_t ms;
} proto_result_t;

typedef enum {
    PROTO_TILE_UNKNOWN, // any "state" but "ok" or "bad", and how a tile looks before its first update
    PROTO_TILE_OK,
    PROTO_TILE_BAD,
} proto_tile_state_t;

typedef struct {
    char btn[PROTO_ID_MAX];
    char text[PROTO_TILE_MAX]; // cut to fit; the daemon sends printable ASCII on one line
    proto_tile_state_t state;
} proto_tile_t;

typedef struct {
    char name[PROTO_NAME_MAX]; // the daemon's host name, truncated, for display only
    char cert[PROTO_CERT_MAX]; // the daemon cert PEM, exactly as sent
    uint8_t c_host[PROTO_HASH_LEN];
} proto_pair_offer_t;

// A validated icon frame. `pixels` points into the frame, which the caller still owns.
typedef struct {
    uint8_t id[PROTO_ICON_ID_LEN];
    uint16_t w;
    uint16_t h;
    const uint8_t *pixels; // w * h * 4 bytes of B, G, R, A (LVGL's ARGB8888), straight alpha
} proto_icon_t;

typedef enum {
    PROTO_REPORT_CRASH, // from a coredump of an earlier boot; every field is set
    PROTO_REPORT_RESET, // from esp_reset_reason() with no dump; only id and reason are sent
} proto_report_kind_t;

// What the device tells the daemon about an earlier crash or reset. Strings are NUL-terminated.
typedef struct {
    char id[PROTO_REPORT_ID_LEN]; // 8 lowercase hex digits
    proto_report_kind_t kind;
    char reason[12]; // panic, int_wdt, task_wdt, wdt or brownout
    char task[17];   // printable ASCII, at most 16 characters, may be empty
    uint32_t pc;
    uint32_t ra;
    char cause[33]; // 1 to 32 printable ASCII characters
    uint32_t addr;
    char elf[65]; // 64 lowercase hex digits
    bool same_build;
} proto_report_t;

typedef struct {
    proto_type_t type;
    char unknown_t[16]; // PROTO_MSG_UNKNOWN: the "t" value, truncated, for logging
    union {
        char welcome_name[PROTO_NAME_MAX];
        struct {
            char code[16];
            char msg[96];
        } error;
        proto_layout_t layout;
        proto_result_t result;
        proto_tile_t tile;
        proto_pair_offer_t pair_offer;
        uint8_t n_host[PROTO_NONCE_LEN];         // PROTO_MSG_PAIR_REVEAL
        bool pair_ok;                            // PROTO_MSG_PAIR_RESULT
        char report_ack_id[PROTO_REPORT_ID_LEN]; // PROTO_MSG_REPORT_ACK, 8 lowercase hex digits
    };
} proto_msg_t;

/** Write "wd-" plus the MAC as 12 lowercase hex digits. `out` must hold PROTO_DEVICE_ID_LEN. */
void proto_device_id(char out[PROTO_DEVICE_ID_LEN], const uint8_t mac[6]);

/**
 * Write the daemon cert fingerprint the daemon advertises in mDNS TXT "fp": the first
 * PROTO_FP_LEN lowercase hex digits of SHA-256 over the cert PEM, NUL excluded.
 */
esp_err_t proto_fp(const char *pem, char out[PROTO_FP_LEN + 1]);

/**
 * Build a hello message carrying `token`. `caps` names the optional features this build fully supports;
 * the key is left out when `n_caps` is 0. Returns the JSON length, or 0 if it does not fit in `buf`.
 */
size_t proto_build_hello(char *buf, size_t buf_len, const char *device, const uint8_t token[PROTO_TOKEN_LEN],
                         const char *fw, int width, int height, const char *const *caps, size_t n_caps);

/** Build pair_req. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_pair_req(char *buf, size_t buf_len, const char *device, const uint8_t commit[PROTO_HASH_LEN]);

/** Build pair_nonce. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_pair_nonce(char *buf, size_t buf_len, const uint8_t n_dev[PROTO_NONCE_LEN]);

/** Build icon_get for `n` ids. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_icon_get(char *buf, size_t buf_len, const uint8_t ids[][PROTO_ICON_ID_LEN], size_t n);

/** Build layout_ack for the layout with revision `rev`. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_layout_ack(char *buf, size_t buf_len, uint32_t rev);

/** Build a press message. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_press(char *buf, size_t buf_len, const char *btn, uint32_t seq);

/**
 * Build a report. The daemon acks and discards a mistyped or malformed one, so this returns 0 for an
 * invalid `id` or `reason`, and for a crash whose `elf` is not 64 lowercase hex digits or whose `task`
 * or `cause` is not printable ASCII of the allowed length. It also returns 0 if the JSON does not fit
 * in `buf`. A reset sends only `id`, `kind` and `reason`.
 */
size_t proto_build_report(char *buf, size_t buf_len, const proto_report_t *r);

/**
 * Parse one daemon message. Unknown fields are ignored. An unknown "t" returns ESP_OK with
 * PROTO_MSG_UNKNOWN. Malformed JSON, a missing "t", or a missing or out-of-range required
 * field returns ESP_ERR_INVALID_ARG, and `out` is then unspecified. Binary pairing fields must
 * be lowercase hex of exactly their length, and a cert that does not fit is refused, never cut.
 */
esp_err_t proto_parse(const char *json, size_t len, proto_msg_t *out);

/**
 * Check one binary frame from the daemon, in this order: at least a header, the "WDI1" magic, width and height
 * each in 1..PROTO_ICON_MAX_SIDE, a length of exactly header + w * h * 4, and the id equal to the first 16 bytes of
 * SHA-256 over the frame from byte 20 on (width, height, pixels). Nothing is read past `len`. Any failure returns
 * a non-ESP_OK code, logs why, and leaves `out` unspecified.
 */
esp_err_t proto_parse_icon(const uint8_t *frame, size_t len, proto_icon_t *out);
