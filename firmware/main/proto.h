#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Wire format: site/src/protocol.md. Every function here is pure (no I/O, no globals). */

#define PROTO_VERSION 1
#define PROTO_MAX_MSG 16384 // one text frame, either direction
#define PROTO_MAX_COLS 8
#define PROTO_MAX_ROWS 5
#define PROTO_MAX_BUTTONS (PROTO_MAX_COLS * PROTO_MAX_ROWS)
#define PROTO_ID_MAX 64
#define PROTO_LABEL_MAX 48
#define PROTO_NAME_MAX 48
#define PROTO_TEXT_MAX 1025    // up to 1 KB of output plus NUL
#define PROTO_DEVICE_ID_LEN 16 // "wd-" + 12 hex + NUL
#define PROTO_FP_LEN 16        // hex digits of the daemon cert fingerprint in mDNS TXT "fp"

typedef enum {
    PROTO_MSG_UNKNOWN, // well-formed JSON with a "t" this firmware does not know
    PROTO_MSG_WELCOME,
    PROTO_MSG_ERROR,
    PROTO_MSG_LAYOUT,
    PROTO_MSG_RESULT,
} proto_type_t;

typedef struct {
    char id[PROTO_ID_MAX];
    char label[PROTO_LABEL_MAX];
    uint8_t col;
    uint8_t row;
    uint32_t bg; // 0xRRGGBB
    uint32_t fg;
} proto_button_t;

typedef struct {
    uint32_t rev;
    uint8_t cols;
    uint8_t rows;
    size_t n_buttons;
    proto_button_t buttons[PROTO_MAX_BUTTONS];
} proto_layout_t;

typedef struct {
    char btn[PROTO_ID_MAX];
    uint32_t seq;
    bool ok;
    int code;
    char text[PROTO_TEXT_MAX];
    uint32_t ms;
} proto_result_t;

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
    };
} proto_msg_t;

/** Write "wd-" plus the MAC as 12 lowercase hex digits. `out` must hold PROTO_DEVICE_ID_LEN. */
void proto_device_id(char out[PROTO_DEVICE_ID_LEN], const uint8_t mac[6]);

/**
 * Write the daemon cert fingerprint the daemon advertises in mDNS TXT "fp": the first
 * PROTO_FP_LEN lowercase hex digits of SHA-256 over the cert PEM, NUL excluded.
 */
esp_err_t proto_fp(const char *pem, char out[PROTO_FP_LEN + 1]);

/** Build a hello message. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_hello(char *buf, size_t buf_len, const char *device, const char *fw, int width, int height);

/** Build a press message. Returns the JSON length, or 0 if it does not fit in `buf`. */
size_t proto_build_press(char *buf, size_t buf_len, const char *btn, uint32_t seq);

/**
 * Parse one daemon message. Unknown fields are ignored. An unknown "t" returns ESP_OK with
 * PROTO_MSG_UNKNOWN. Malformed JSON, a missing "t", or a missing or out-of-range required
 * field returns ESP_ERR_INVALID_ARG, and `out` is then unspecified.
 */
esp_err_t proto_parse(const char *json, size_t len, proto_msg_t *out);
