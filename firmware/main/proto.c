#include "proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "proto";

void proto_device_id(char out[PROTO_DEVICE_ID_LEN], const uint8_t mac[6])
{
    snprintf(out, PROTO_DEVICE_ID_LEN, "wd-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t proto_fp(const char *pem, char out[PROTO_FP_LEN + 1])
{
    uint8_t hash[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t hash_len;
    psa_status_t status =
        psa_hash_compute(PSA_ALG_SHA_256, (const uint8_t *)pem, strlen(pem), hash, sizeof(hash), &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute: %d", (int)status);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < PROTO_FP_LEN / 2; i++) {
        snprintf(out + 2 * i, 3, "%02x", hash[i]);
    }
    return ESP_OK;
}

// Print `root` into `buf` and free it. Returns the length, or 0 if it does not fit.
static size_t print_into(cJSON *root, char *buf, size_t buf_len)
{
    size_t len = 0;
    if (root && cJSON_PrintPreallocated(root, buf, (int)buf_len, false)) {
        len = strlen(buf);
    }
    cJSON_Delete(root);
    return len;
}

size_t proto_build_hello(char *buf, size_t buf_len, const char *device, const char *fw, int width, int height)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *screen = cJSON_CreateArray();
    if (!root || !screen) {
        cJSON_Delete(screen);
        cJSON_Delete(root);
        return 0;
    }
    cJSON_AddItemToArray(screen, cJSON_CreateNumber(width));
    cJSON_AddItemToArray(screen, cJSON_CreateNumber(height));
    cJSON_AddStringToObject(root, "t", "hello");
    cJSON_AddNumberToObject(root, "v", PROTO_VERSION);
    cJSON_AddStringToObject(root, "device", device);
    cJSON_AddStringToObject(root, "fw", fw);
    cJSON_AddItemToObject(root, "screen", screen);
    return print_into(root, buf, buf_len);
}

size_t proto_build_press(char *buf, size_t buf_len, const char *btn, uint32_t seq)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "press");
    cJSON_AddStringToObject(root, "btn", btn);
    cJSON_AddNumberToObject(root, "seq", seq);
    return print_into(root, buf, buf_len);
}

// Copy a string field into dst. Missing or non-string fields return false. A value that does
// not fit is truncated, or rejected when `exact` is set (ids must survive the round trip).
static bool get_str(const cJSON *obj, const char *key, char *dst, size_t dst_len, bool exact)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    if (exact && strlen(item->valuestring) >= dst_len) {
        return false;
    }
    strlcpy(dst, item->valuestring, dst_len);
    return true;
}

// Read an integer field in [min, max].
static bool get_int(const cJSON *obj, const char *key, double min, double max, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        return false;
    }
    *out = item->valuedouble;
    return true;
}

static esp_err_t parse_layout(const cJSON *root, proto_layout_t *out)
{
    double rev, cols, rows;
    const cJSON *buttons = cJSON_GetObjectItemCaseSensitive(root, "buttons");
    if (!get_int(root, "rev", 0, UINT32_MAX, &rev) || !get_int(root, "cols", 1, PROTO_MAX_COLS, &cols) ||
        !get_int(root, "rows", 1, PROTO_MAX_ROWS, &rows) || !cJSON_IsArray(buttons) ||
        cJSON_GetArraySize(buttons) > PROTO_MAX_BUTTONS) {
        return ESP_ERR_INVALID_ARG;
    }
    out->rev = (uint32_t)rev;
    out->cols = (uint8_t)cols;
    out->rows = (uint8_t)rows;
    out->n_buttons = 0;

    const cJSON *item;
    cJSON_ArrayForEach(item, buttons)
    {
        proto_button_t *b = &out->buttons[out->n_buttons];
        double col, row, bg, fg;
        if (!cJSON_IsObject(item) || !get_str(item, "id", b->id, sizeof(b->id), true) || b->id[0] == '\0' ||
            !get_str(item, "label", b->label, sizeof(b->label), false) || !get_int(item, "col", 0, cols - 1, &col) ||
            !get_int(item, "row", 0, rows - 1, &row) || !get_int(item, "bg", 0, 0xFFFFFF, &bg) ||
            !get_int(item, "fg", 0, 0xFFFFFF, &fg)) {
            return ESP_ERR_INVALID_ARG;
        }
        b->col = (uint8_t)col;
        b->row = (uint8_t)row;
        b->bg = (uint32_t)bg;
        b->fg = (uint32_t)fg;
        out->n_buttons++;
    }
    return ESP_OK;
}

static esp_err_t parse_result(const cJSON *root, proto_result_t *out)
{
    double seq, code, ms;
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (!cJSON_IsBool(ok) || !get_int(root, "seq", 0, UINT32_MAX, &seq) || !get_int(root, "code", -1e9, 1e9, &code) ||
        !get_int(root, "ms", 0, UINT32_MAX, &ms)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->ok = cJSON_IsTrue(ok);
    out->seq = (uint32_t)seq;
    out->code = (int)code;
    out->ms = (uint32_t)ms;
    get_str(root, "btn", out->btn, sizeof(out->btn), false); // optional, only logged
    get_str(root, "text", out->text, sizeof(out->text), false);
    return ESP_OK;
}

esp_err_t proto_parse(const char *json, size_t len, proto_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        ESP_LOGW(TAG, "malformed JSON");
        return ESP_ERR_INVALID_ARG;
    }

    char t[16];
    esp_err_t err = ESP_OK;
    if (!cJSON_IsObject(root) || !get_str(root, "t", t, sizeof(t), false)) {
        err = ESP_ERR_INVALID_ARG;
    } else if (strcmp(t, "welcome") == 0) {
        out->type = PROTO_MSG_WELCOME;
        if (!get_str(root, "name", out->welcome_name, sizeof(out->welcome_name), false)) {
            strlcpy(out->welcome_name, "daemon", sizeof(out->welcome_name));
        }
    } else if (strcmp(t, "error") == 0) {
        out->type = PROTO_MSG_ERROR;
        get_str(root, "code", out->error.code, sizeof(out->error.code), false);
        get_str(root, "msg", out->error.msg, sizeof(out->error.msg), false);
    } else if (strcmp(t, "layout") == 0) {
        out->type = PROTO_MSG_LAYOUT;
        err = parse_layout(root, &out->layout);
    } else if (strcmp(t, "result") == 0) {
        out->type = PROTO_MSG_RESULT;
        err = parse_result(root, &out->result);
    } else {
        out->type = PROTO_MSG_UNKNOWN;
        strlcpy(out->unknown_t, t, sizeof(out->unknown_t));
    }
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "invalid message");
    }
    return err;
}
