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

// Write n bytes as 2n lowercase hex digits plus NUL.
static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xf];
    }
    out[2 * n] = '\0';
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
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

size_t proto_build_hello(char *buf, size_t buf_len, const char *device, const uint8_t token[PROTO_TOKEN_LEN],
                         const char *fw, int width, int height)
{
    char token_hex[2 * PROTO_TOKEN_LEN + 1];
    to_hex(token, PROTO_TOKEN_LEN, token_hex);
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
    cJSON_AddStringToObject(root, "token", token_hex);
    cJSON_AddStringToObject(root, "fw", fw);
    cJSON_AddItemToObject(root, "screen", screen);
    return print_into(root, buf, buf_len);
}

size_t proto_build_pair_req(char *buf, size_t buf_len, const char *device, const uint8_t commit[PROTO_HASH_LEN])
{
    char commit_hex[2 * PROTO_HASH_LEN + 1];
    to_hex(commit, PROTO_HASH_LEN, commit_hex);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "pair_req");
    cJSON_AddNumberToObject(root, "v", PROTO_VERSION);
    cJSON_AddStringToObject(root, "device", device);
    cJSON_AddStringToObject(root, "commit", commit_hex);
    return print_into(root, buf, buf_len);
}

size_t proto_build_pair_nonce(char *buf, size_t buf_len, const uint8_t n_dev[PROTO_NONCE_LEN])
{
    char n_dev_hex[2 * PROTO_NONCE_LEN + 1];
    to_hex(n_dev, PROTO_NONCE_LEN, n_dev_hex);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "pair_nonce");
    cJSON_AddStringToObject(root, "n_dev", n_dev_hex);
    return print_into(root, buf, buf_len);
}

size_t proto_build_icon_get(char *buf, size_t buf_len, const uint8_t ids[][PROTO_ICON_ID_LEN], size_t n)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    if (!root || !list) {
        cJSON_Delete(list);
        cJSON_Delete(root);
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char hex[2 * PROTO_ICON_ID_LEN + 1];
        to_hex(ids[i], PROTO_ICON_ID_LEN, hex);
        cJSON_AddItemToArray(list, cJSON_CreateString(hex));
    }
    cJSON_AddStringToObject(root, "t", "icon_get");
    cJSON_AddItemToObject(root, "ids", list);
    return print_into(root, buf, buf_len);
}

static bool is_hex(const char *s, size_t n)
{
    if (strlen(s) != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (hex_digit(s[i]) < 0) {
            return false;
        }
    }
    return true;
}

static bool is_printable(const char *s, size_t min, size_t max)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7e) {
            return false;
        }
    }
    return n >= min && n <= max;
}

static void add_addr(cJSON *root, const char *key, uint32_t v)
{
    char hex[11];
    snprintf(hex, sizeof(hex), "0x%08x", (unsigned)v);
    cJSON_AddStringToObject(root, key, hex);
}

size_t proto_build_report(char *buf, size_t buf_len, const proto_report_t *r)
{
    static const char *const reasons[] = {"panic", "int_wdt", "task_wdt", "wdt", "brownout"};
    bool reason_ok = false;
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        reason_ok |= strcmp(r->reason, reasons[i]) == 0;
    }
    bool crash = r->kind == PROTO_REPORT_CRASH;
    if (!reason_ok || !is_hex(r->id, 8) ||
        (crash && (!is_hex(r->elf, 64) || !is_printable(r->task, 0, 16) || !is_printable(r->cause, 1, 32)))) {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "report");
    cJSON_AddStringToObject(root, "id", r->id);
    cJSON_AddStringToObject(root, "kind", crash ? "crash" : "reset");
    cJSON_AddStringToObject(root, "reason", r->reason);
    if (crash) {
        cJSON_AddStringToObject(root, "task", r->task);
        add_addr(root, "pc", r->pc);
        add_addr(root, "ra", r->ra);
        cJSON_AddStringToObject(root, "cause", r->cause);
        add_addr(root, "addr", r->addr);
        cJSON_AddStringToObject(root, "elf", r->elf);
        cJSON_AddBoolToObject(root, "same_build", r->same_build);
    }
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

size_t proto_build_layout_ack(char *buf, size_t buf_len, uint32_t rev)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "layout_ack");
    cJSON_AddNumberToObject(root, "rev", rev);
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

// Decode a string field of exactly 2n lowercase hex digits into n bytes.
static bool get_hex(const cJSON *obj, const char *key, uint8_t *dst, size_t n)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_digit(item->valuestring[2 * i]);
        int lo = hex_digit(item->valuestring[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        dst[i] = (uint8_t)(hi << 4 | lo);
    }
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
        const cJSON *icon = cJSON_GetObjectItemCaseSensitive(item, "icon");
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
        // No icon key leaves the button plain. A key must hold a well-formed id, so null is refused.
        b->has_icon = icon != NULL;
        if (b->has_icon && !get_hex(item, "icon", b->icon, sizeof(b->icon))) {
            return ESP_ERR_INVALID_ARG;
        }
        // Absent means false. A key must be a bool.
        const cJSON *tile = cJSON_GetObjectItemCaseSensitive(item, "tile");
        if (tile && !cJSON_IsBool(tile)) {
            return ESP_ERR_INVALID_ARG;
        }
        b->tile = cJSON_IsTrue(tile);
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
    get_str(root, "btn", out->btn, sizeof(out->btn), false); // optional, names the button on screen
    get_str(root, "summary", out->summary, sizeof(out->summary), false);
    get_str(root, "text", out->text, sizeof(out->text), false);
    return ESP_OK;
}

static esp_err_t parse_tile(const cJSON *root, proto_tile_t *out)
{
    if (!get_str(root, "btn", out->btn, sizeof(out->btn), true) ||
        !get_str(root, "text", out->text, sizeof(out->text), false)) {
        return ESP_ERR_INVALID_ARG;
    }
    char state[8] = "";
    get_str(root, "state", state, sizeof(state), false);
    out->state = strcmp(state, "ok") == 0    ? PROTO_TILE_OK
                 : strcmp(state, "bad") == 0 ? PROTO_TILE_BAD
                                             : PROTO_TILE_UNKNOWN;
    return ESP_OK;
}

static esp_err_t parse_pair_offer(const cJSON *root, proto_pair_offer_t *out)
{
    if (!get_str(root, "name", out->name, sizeof(out->name), false) ||
        !get_str(root, "cert", out->cert, sizeof(out->cert), true) ||
        !get_hex(root, "c_host", out->c_host, sizeof(out->c_host))) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t proto_parse_icon(const uint8_t *frame, size_t len, proto_icon_t *out)
{
    if (len < PROTO_ICON_HEADER_LEN) {
        ESP_LOGW(TAG, "icon frame of %u bytes is shorter than its header", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(frame, "WDI1", 4) != 0) {
        ESP_LOGW(TAG, "binary frame is not an icon");
        return ESP_ERR_INVALID_ARG;
    }
    size_t w = frame[20] | (size_t)frame[21] << 8;
    size_t h = frame[22] | (size_t)frame[23] << 8;
    if (w < 1 || w > PROTO_ICON_MAX_SIDE || h < 1 || h > PROTO_ICON_MAX_SIDE) {
        ESP_LOGW(TAG, "icon of %ux%u is outside 1..%d", (unsigned)w, (unsigned)h, PROTO_ICON_MAX_SIDE);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len != PROTO_ICON_HEADER_LEN + w * h * 4) {
        ESP_LOGW(TAG, "icon of %ux%u should be %u bytes, got %u", (unsigned)w, (unsigned)h,
                 (unsigned)(PROTO_ICON_HEADER_LEN + w * h * 4), (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t hash[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t hash_len;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, frame + 20, len - 20, hash, sizeof(hash), &hash_len);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_compute: %d", (int)status);
        return ESP_FAIL;
    }
    if (memcmp(hash, frame + 4, PROTO_ICON_ID_LEN) != 0) {
        ESP_LOGW(TAG, "icon id does not match its pixels");
        return ESP_ERR_INVALID_CRC;
    }
    memcpy(out->id, frame + 4, PROTO_ICON_ID_LEN);
    out->w = (uint16_t)w;
    out->h = (uint16_t)h;
    out->pixels = frame + PROTO_ICON_HEADER_LEN;
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
    } else if (strcmp(t, "tile") == 0) {
        out->type = PROTO_MSG_TILE;
        err = parse_tile(root, &out->tile);
    } else if (strcmp(t, "pair_offer") == 0) {
        out->type = PROTO_MSG_PAIR_OFFER;
        err = parse_pair_offer(root, &out->pair_offer);
    } else if (strcmp(t, "pair_reveal") == 0) {
        out->type = PROTO_MSG_PAIR_REVEAL;
        err = get_hex(root, "n_host", out->n_host, sizeof(out->n_host)) ? ESP_OK : ESP_ERR_INVALID_ARG;
    } else if (strcmp(t, "pair_result") == 0) {
        out->type = PROTO_MSG_PAIR_RESULT;
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
        err = cJSON_IsBool(ok) ? ESP_OK : ESP_ERR_INVALID_ARG;
        out->pair_ok = cJSON_IsTrue(ok);
    } else if (strcmp(t, "report_ack") == 0) {
        out->type = PROTO_MSG_REPORT_ACK;
        bool ok =
            get_str(root, "id", out->report_ack_id, sizeof(out->report_ack_id), true) && is_hex(out->report_ack_id, 8);
        err = ok ? ESP_OK : ESP_ERR_INVALID_ARG;
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
