#include "pairing.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/x509_crt.h"
#include "nvs.h"
#include "psa/crypto.h"

#define NVS_NAMESPACE "watchdeck"
#define NVS_KEY "pairing"
#define BLOB_VERSION 1
#define CODE_LABEL "watchdeck-pair-v1"
#define PEM_HEADER "-----BEGIN CERTIFICATE-----"

static const char *TAG = "pairing";

enum { STAGE_IDLE, STAGE_OFFER, STAGE_REVEAL, STAGE_RESULT, STAGE_DONE };

typedef struct {
    uint32_t version;
    pairing_t pairing;
} blob_t;

static bool terminated(const char *s, size_t len)
{
    return memchr(s, '\0', len) != NULL;
}

esp_err_t pairing_load(pairing_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND; // the namespace appears with the first save
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");
    blob_t *blob = heap_caps_malloc(sizeof(*blob), MALLOC_CAP_SPIRAM);
    if (!blob) {
        nvs_close(h);
        return ESP_ERR_NO_MEM;
    }
    size_t len = sizeof(*blob);
    err = nvs_get_blob(h, NVS_KEY, blob, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_ERR_NOT_FOUND;
    } else if (err == ESP_OK && (len != sizeof(*blob) || blob->version != BLOB_VERSION ||
                                 !terminated(blob->pairing.name, sizeof(blob->pairing.name)) ||
                                 !terminated(blob->pairing.cert_pem, sizeof(blob->pairing.cert_pem)))) {
        err = ESP_ERR_INVALID_STATE;
    }
    if (err == ESP_OK) {
        *out = blob->pairing;
    }
    memset(blob, 0, sizeof(*blob));
    free(blob);
    return err;
}

esp_err_t pairing_save(const pairing_t *pairing)
{
    blob_t *blob = heap_caps_calloc(1, sizeof(*blob), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(blob, ESP_ERR_NO_MEM, TAG, "alloc");
    blob->version = BLOB_VERSION;
    blob->pairing = *pairing;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, blob, sizeof(*blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    memset(blob, 0, sizeof(*blob));
    free(blob);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving the pairing: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t pairing_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erasing the pairing: %s", esp_err_to_name(err));
    }
    return err;
}

static bool hash_field(psa_hash_operation_t *op, const void *data, size_t len)
{
    const uint8_t be_len[2] = {(uint8_t)(len >> 8), (uint8_t)len};
    return len <= 0xFFFF && psa_hash_update(op, be_len, sizeof(be_len)) == PSA_SUCCESS &&
           psa_hash_update(op, data, len) == PSA_SUCCESS;
}

esp_err_t pairing_code(const char *device, const uint8_t commit[PROTO_HASH_LEN], const char *cert_pem,
                       const uint8_t n_dev[PROTO_NONCE_LEN], const uint8_t n_host[PROTO_NONCE_LEN],
                       char out[PAIRING_CODE_LEN + 1])
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    uint8_t hash[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t hash_len;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS && hash_field(&op, CODE_LABEL, strlen(CODE_LABEL)) &&
              hash_field(&op, device, strlen(device)) && hash_field(&op, commit, PROTO_HASH_LEN) &&
              hash_field(&op, cert_pem, strlen(cert_pem)) && hash_field(&op, n_dev, PROTO_NONCE_LEN) &&
              hash_field(&op, n_host, PROTO_NONCE_LEN) &&
              psa_hash_finish(&op, hash, sizeof(hash), &hash_len) == PSA_SUCCESS;
    if (!ok) {
        psa_hash_abort(&op);
        return ESP_FAIL;
    }
    uint32_t n = (uint32_t)hash[0] << 24 | (uint32_t)hash[1] << 16 | (uint32_t)hash[2] << 8 | hash[3];
    snprintf(out, PAIRING_CODE_LEN + 1, "%06u", (unsigned)(n % 1000000u));
    return ESP_OK;
}

static bool sha256(const uint8_t *in, size_t len, uint8_t out[PROTO_HASH_LEN])
{
    size_t out_len;
    return psa_hash_compute(PSA_ALG_SHA_256, in, len, out, PROTO_HASH_LEN, &out_len) == PSA_SUCCESS;
}

// Exactly one PEM certificate that mbedTLS parses, as esp-tls must at every connect.
static bool cert_valid(const char *pem)
{
    if (strncmp(pem, PEM_HEADER, strlen(PEM_HEADER)) != 0) {
        return false;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    bool ok = mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0 && crt.next == NULL;
    mbedtls_x509_crt_free(&crt);
    return ok;
}

static pairing_step_t fail(pairing_session_t *ps, const char *why)
{
    strlcpy(ps->error, why, sizeof(ps->error));
    ps->stage = STAGE_IDLE;
    return PAIRING_FAILED;
}

size_t pairing_begin(pairing_session_t *ps, const char *device, char *buf, size_t buf_len)
{
    memset(ps, 0, sizeof(*ps));
    strlcpy(ps->device, device, sizeof(ps->device));
    if (psa_generate_random(ps->result.token, PROTO_TOKEN_LEN) != PSA_SUCCESS ||
        psa_generate_random(ps->n_dev, PROTO_NONCE_LEN) != PSA_SUCCESS ||
        !sha256(ps->result.token, PROTO_TOKEN_LEN, ps->commit)) {
        fail(ps, "no randomness");
        return 0;
    }
    size_t len = proto_build_pair_req(buf, buf_len, device, ps->commit);
    if (len == 0) {
        fail(ps, "pair_req does not fit");
        return 0;
    }
    ps->stage = STAGE_OFFER;
    return len;
}

pairing_step_t pairing_feed(pairing_session_t *ps, const proto_msg_t *m, char *buf, size_t buf_len, size_t *reply_len)
{
    *reply_len = 0;
    if (m->type == PROTO_MSG_ERROR) {
        char why[sizeof(ps->error)];
        snprintf(why, sizeof(why), "daemon error %s", m->error.code);
        return fail(ps, why);
    }
    switch (ps->stage) {
    case STAGE_OFFER:
        if (m->type != PROTO_MSG_PAIR_OFFER) {
            return fail(ps, "unexpected message");
        }
        if (!cert_valid(m->pair_offer.cert)) {
            return fail(ps, "invalid daemon cert");
        }
        strlcpy(ps->result.name, m->pair_offer.name, sizeof(ps->result.name));
        strlcpy(ps->result.cert_pem, m->pair_offer.cert, sizeof(ps->result.cert_pem));
        memcpy(ps->c_host, m->pair_offer.c_host, PROTO_HASH_LEN);
        *reply_len = proto_build_pair_nonce(buf, buf_len, ps->n_dev);
        if (*reply_len == 0) {
            return fail(ps, "pair_nonce does not fit");
        }
        ps->stage = STAGE_REVEAL;
        return PAIRING_REPLY;
    case STAGE_REVEAL: {
        if (m->type != PROTO_MSG_PAIR_REVEAL) {
            return fail(ps, "unexpected message");
        }
        // The daemon committed to n_host before it saw n_dev, so it can't steer the code.
        uint8_t check[PROTO_HASH_LEN];
        if (!sha256(m->n_host, PROTO_NONCE_LEN, check) || memcmp(check, ps->c_host, PROTO_HASH_LEN) != 0) {
            return fail(ps, "daemon broke its commitment");
        }
        if (pairing_code(ps->device, ps->commit, ps->result.cert_pem, ps->n_dev, m->n_host, ps->code) != ESP_OK) {
            return fail(ps, "could not derive the code");
        }
        ps->stage = STAGE_RESULT;
        return PAIRING_CODE;
    }
    case STAGE_RESULT:
        if (m->type != PROTO_MSG_PAIR_RESULT) {
            return fail(ps, "unexpected message");
        }
        if (!m->pair_ok) {
            return fail(ps, "the desktop did not confirm");
        }
        ps->stage = STAGE_DONE;
        return PAIRING_DONE;
    default:
        return fail(ps, "no attempt in progress");
    }
}

void pairing_end(pairing_session_t *ps)
{
    memset(ps, 0, sizeof(*ps));
}
