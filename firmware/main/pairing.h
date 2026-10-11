#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "proto.h"

/*
 * Pairing (site/src/protocol.md): the commit/reveal exchange on a daemon's pairing port, the
 * code both sides derive from it, and the NVS blob that keeps the result. The device draws its
 * own token and sends only SHA-256(token), so nothing secret crosses the plain pairing socket.
 */

#define PAIRING_CODE_LEN 6

/** What a paired device keeps: its token and the daemon it opens. */
typedef struct {
    uint8_t token[PROTO_TOKEN_LEN];
    char name[PROTO_NAME_MAX];     // the daemon's host name, for display
    char cert_pem[PROTO_CERT_MAX]; // the daemon cert, pinned byte for byte
} pairing_t;

/** Load the pairing from NVS. ESP_ERR_NOT_FOUND when unpaired; other errors mean an unreadable blob. */
esp_err_t pairing_load(pairing_t *out);

/** Store the pairing as one NVS blob, which NVS writes atomically. */
esp_err_t pairing_save(const pairing_t *pairing);

/** Erase the pairing from NVS. Erasing when unpaired succeeds. */
esp_err_t pairing_erase(void);

/**
 * The 6-digit code: the first 4 bytes of SHA-256 over every input, as a big-endian u32, mod
 * 10^6, zero-padded. Each input goes in as a u16-BE length and its bytes.
 */
esp_err_t pairing_code(const char *device, const uint8_t commit[PROTO_HASH_LEN], const char *cert_pem,
                       const uint8_t n_dev[PROTO_NONCE_LEN], const uint8_t n_host[PROTO_NONCE_LEN],
                       char out[PAIRING_CODE_LEN + 1]);

typedef enum {
    PAIRING_REPLY,  // send the reply left in the buffer
    PAIRING_CODE,   // show `code` and wait for the daemon's verdict
    PAIRING_DONE,   // the daemon says paired: save `result` only once the person confirms
    PAIRING_FAILED, // `error` says why
} pairing_step_t;

/** One pairing attempt. It holds a cert, so keep it off the stack. */
typedef struct {
    int stage;
    char device[PROTO_DEVICE_ID_LEN];
    uint8_t commit[PROTO_HASH_LEN];
    uint8_t n_dev[PROTO_NONCE_LEN];
    uint8_t c_host[PROTO_HASH_LEN];
    char code[PAIRING_CODE_LEN + 1];
    char error[48];
    pairing_t result;
} pairing_session_t;

/**
 * Start an attempt as `device`: draw the token and nonce, and build pair_req into `buf`.
 * Returns its length, or 0 on failure with `error` set.
 */
size_t pairing_begin(pairing_session_t *ps, const char *device, char *buf, size_t buf_len);

/**
 * Feed the daemon's next message, already parsed and not PROTO_MSG_UNKNOWN. On PAIRING_REPLY
 * the reply is in `buf`, `*reply_len` bytes long. Every field was validated by proto_parse;
 * this checks the cert parses, the reveal matches its commitment, and the order is right.
 */
pairing_step_t pairing_feed(pairing_session_t *ps, const proto_msg_t *m, char *buf, size_t buf_len, size_t *reply_len);

/** End an attempt and wipe its token. */
void pairing_end(pairing_session_t *ps);
