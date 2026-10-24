#include "proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "psa/crypto.h"

static const char *TAG = "proto";

// 2^53 - 1, the largest whole number a JSON number in a double carries exactly.
#define STATS_INT_MAX 9007199254740991.0

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
                         const char *fw, int width, int height, const char *const *caps, size_t n_caps)
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
    if (n_caps > 0) {
        cJSON *list = cJSON_CreateStringArray(caps, (int)n_caps);
        if (!list) {
            cJSON_Delete(root);
            return 0;
        }
        cJSON_AddItemToObject(root, "caps", list);
    }
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

size_t proto_build_asset_get(char *buf, size_t buf_len, const uint8_t id[PROTO_ASSET_ID_LEN], uint32_t offset)
{
    if (offset % PROTO_ASSET_CHUNK != 0) {
        return 0;
    }
    char id_hex[2 * PROTO_ASSET_ID_LEN + 1];
    to_hex(id, PROTO_ASSET_ID_LEN, id_hex);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "asset_get");
    cJSON_AddStringToObject(root, "id", id_hex);
    cJSON_AddNumberToObject(root, "offset", offset);
    return print_into(root, buf, buf_len);
}

size_t proto_build_feed_dismiss(char *buf, size_t buf_len, const char *id)
{
    if (!is_printable(id, 1, PROTO_FEED_ID_MAX - 1)) {
        return 0;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return 0;
    }
    cJSON_AddStringToObject(root, "t", "feed_dismiss");
    cJSON_AddStringToObject(root, "id", id);
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

// Read an integer field in [min, max] that has no fraction. get_int truncates a fraction, which layouts accept.
static bool get_whole(const cJSON *obj, const char *key, double min, double max, double *out)
{
    return get_int(obj, key, min, max, out) && *out == (double)(int64_t)*out;
}

// Read two integer fields, each in [0, STATS_INT_MAX], that come together or not at all. `*present` is
// set when both are there. Returns false for one without the other, or for a value that is not a whole number.
static bool get_pair(const cJSON *obj, const char *a, const char *b, uint64_t out[2], bool *present)
{
    bool has_a = cJSON_GetObjectItemCaseSensitive(obj, a) != NULL;
    bool has_b = cJSON_GetObjectItemCaseSensitive(obj, b) != NULL;
    *present = has_a && has_b;
    if (has_a != has_b) {
        return false;
    }
    if (!*present) {
        return true;
    }
    double va, vb;
    if (!get_whole(obj, a, 0, STATS_INT_MAX, &va) || !get_whole(obj, b, 0, STATS_INT_MAX, &vb)) {
        return false;
    }
    out[0] = (uint64_t)va;
    out[1] = (uint64_t)vb;
    return true;
}

// Read a font field. Absent gives `inherit`. A known name gives that font. Anything else (an unknown
// name, a non-string, an over-long string) gives Montserrat and sets `*bad`: a font is cosmetic, so the
// layout is still accepted. The first such field in a layout is logged.
static wd_font_t get_font(const cJSON *obj, const char *key, wd_font_t inherit, bool *bad)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!item) {
        return inherit;
    }
    bool known = false;
    wd_font_t font = wd_font_from_name(cJSON_IsString(item) ? item->valuestring : NULL, &known);
    if (!known && !*bad) {
        *bad = true;
        ESP_LOGW(TAG, "layout field \"%s\" is not a known font, drawing it in montserrat", key);
    }
    return font;
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
    // Page fields come together or not at all. No `page` is a set of one page with no id.
    const cJSON *page_item = cJSON_GetObjectItemCaseSensitive(root, "page");
    if (page_item) {
        double page, pages;
        if (!get_int(root, "page", 0, PROTO_MAX_PAGES - 1, &page) ||
            !get_int(root, "pages", 1, PROTO_MAX_PAGES, &pages) || page >= pages ||
            !get_str(root, "page_id", out->page_id, sizeof(out->page_id), true) || out->page_id[0] == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        out->page = (uint8_t)page;
        out->pages = (uint8_t)pages;
    } else if (cJSON_GetObjectItemCaseSensitive(root, "pages") || cJSON_GetObjectItemCaseSensitive(root, "page_id")) {
        return ESP_ERR_INVALID_ARG;
    } else {
        out->page = 0;
        out->pages = 1;
        out->page_id[0] = '\0';
    }
    out->rev = (uint32_t)rev;
    out->cols = (uint8_t)cols;
    out->rows = (uint8_t)rows;
    out->n_buttons = 0;
    out->unknown_font = false;
    out->font = get_font(root, "font", WD_FONT_MONTSERRAT, &out->unknown_font);

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
        b->label_font = get_font(item, "label_font", out->font, &out->unknown_font);
        b->value_font = get_font(item, "value_font", b->label_font, &out->unknown_font);
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

// Read a unit field from `names`, giving its index in `*out`. Absent leaves `*out` as the caller set it. A
// non-string, or a string not in `names`, is refused.
static bool get_unit(const cJSON *obj, const char *key, const char *const names[], size_t n, int *out)
{
    if (!cJSON_GetObjectItemCaseSensitive(obj, key)) {
        return true;
    }
    char s[8] = "";
    if (!get_str(obj, key, s, sizeof(s), true)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

// Read the colors object. Each known key that is present must be a whole 0xRRGGBB, and unknown keys are
// ignored. An empty object is valid and sets no bits.
static esp_err_t parse_stats_colors(const cJSON *root, proto_stats_t *out)
{
    static const char *const keys[PROTO_COLOR_COUNT] = {"cpu",      "mem",    "gpu",   "cpu_temp",
                                                        "gpu_temp", "net_rx", "net_tx"};
    out->colors_mask = 0;
    const cJSON *colors = cJSON_GetObjectItemCaseSensitive(root, "colors");
    if (!colors) {
        return ESP_OK;
    }
    if (!cJSON_IsObject(colors)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < PROTO_COLOR_COUNT; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(colors, keys[i])) {
            continue;
        }
        double c;
        if (!get_whole(colors, keys[i], 0, 0xFFFFFF, &c)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->colors[i] = (uint32_t)c;
        out->colors_mask |= (uint8_t)(1u << i);
    }
    return ESP_OK;
}

static esp_err_t parse_stats(const cJSON *root, proto_stats_t *out)
{
    double v;
    if (!get_whole(root, "time", 0, STATS_INT_MAX, &v)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->time = (uint64_t)v;

    out->has_utc_offset = cJSON_GetObjectItemCaseSensitive(root, "utc_offset") != NULL;
    if (out->has_utc_offset) {
        if (!get_whole(root, "utc_offset", -64800, 64800, &v)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->utc_offset = (int32_t)v;
    }

    out->has_cpu = cJSON_GetObjectItemCaseSensitive(root, "cpu") != NULL;
    if (out->has_cpu) {
        if (!get_whole(root, "cpu", 0, 100, &v)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->cpu = (uint8_t)v;
    }

    uint64_t mem[2] = {0};
    bool has_mem;
    if (!get_pair(root, "mem_used", "mem_total", mem, &has_mem) || (has_mem && mem[0] > mem[1])) {
        return ESP_ERR_INVALID_ARG;
    }
    out->has_mem = has_mem;
    out->mem_used = mem[0];
    out->mem_total = mem[1];

    uint64_t net[2] = {0};
    bool has_net;
    if (!get_pair(root, "net_rx", "net_tx", net, &has_net)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->has_net = has_net;
    out->net_rx = net[0];
    out->net_tx = net[1];

    // GPU busy and the two temperatures are independent, so a GPU without a sensor still shows its busy share.
    out->has_gpu = cJSON_GetObjectItemCaseSensitive(root, "gpu") != NULL;
    if (out->has_gpu) {
        if (!get_whole(root, "gpu", 0, 100, &v)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->gpu = (uint8_t)v;
    }

    out->has_cpu_temp = cJSON_GetObjectItemCaseSensitive(root, "cpu_temp") != NULL;
    if (out->has_cpu_temp) {
        if (!get_whole(root, "cpu_temp", -40, 150, &v)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->cpu_temp = (int16_t)v;
    }

    out->has_gpu_temp = cJSON_GetObjectItemCaseSensitive(root, "gpu_temp") != NULL;
    if (out->has_gpu_temp) {
        if (!get_whole(root, "gpu_temp", -40, 150, &v)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->gpu_temp = (int16_t)v;
    }

    static const char *const temp_names[] = {"c", "f"};
    static const char *const net_names[] = {"si", "binary", "bits"};
    int temp_unit = PROTO_TEMP_C;
    int net_unit = PROTO_NET_SI;
    if (!get_unit(root, "temp_unit", temp_names, 2, &temp_unit) ||
        !get_unit(root, "net_unit", net_names, 3, &net_unit)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->temp_unit = (proto_temp_unit_t)temp_unit;
    out->net_unit = (proto_net_unit_t)net_unit;

    return parse_stats_colors(root, out);
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

// The index of `s` in `names`, or -1.
static int name_index(const char *s, const char *const names[], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(s, names[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Read a required string field that must be one of `names`. Returns its index, or -1.
static int get_name(const cJSON *obj, const char *key, const char *const names[], size_t n)
{
    char s[16] = "";
    if (!get_str(obj, key, s, sizeof(s), true)) {
        return -1;
    }
    return name_index(s, names, n);
}

// Copy a required printable string of at least `min` characters into dst. The limit is dst_len - 1 characters,
// exactly: a longer value is refused, not cut.
static bool get_text(const cJSON *obj, const char *key, char *dst, size_t dst_len, size_t min)
{
    return get_str(obj, key, dst, dst_len, true) && is_printable(dst, min, dst_len - 1);
}

static esp_err_t parse_sleep(const cJSON *root, proto_sleep_t *out)
{
    static const char *const anchors[] = {"center", "top_left", "top_right", "bottom_left", "bottom_right"};
    static const char *const spinners[] = {"arc", "dots", "bar", "pulse", "none"};
    static const char *const shows[] = {"clock", "date", "host", "text"};
    double bg, fg, spinner_color, offline_after_s, backlight;
    int anchor = get_name(root, "anchor", anchors, 5);
    int spinner = get_name(root, "spinner", spinners, 5);
    if (!get_whole(root, "bg", 0, 0xFFFFFF, &bg) || !get_whole(root, "fg", 0, 0xFFFFFF, &fg) ||
        !get_whole(root, "spinner_color", 0, 0xFFFFFF, &spinner_color) ||
        !get_whole(root, "offline_after_s", 0, 3600, &offline_after_s) ||
        !get_whole(root, "backlight", 0, 100, &backlight) || anchor < 0 || spinner < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    out->has_image = cJSON_GetObjectItemCaseSensitive(root, "image") != NULL;
    if (out->has_image && !get_hex(root, "image", out->image, sizeof(out->image))) {
        return ESP_ERR_INVALID_ARG;
    }

    // Each known name at most once. An unknown name is ignored, so a newer daemon's element doesn't refuse the frame.
    const cJSON *show = cJSON_GetObjectItemCaseSensitive(root, "show");
    if (!cJSON_IsArray(show)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->show_mask = 0;
    const cJSON *name;
    cJSON_ArrayForEach(name, show)
    {
        if (!cJSON_IsString(name)) {
            return ESP_ERR_INVALID_ARG;
        }
        int bit = name_index(name->valuestring, shows, sizeof(shows) / sizeof(shows[0]));
        if (bit < 0) {
            continue;
        }
        if (out->show_mask & (1u << bit)) {
            return ESP_ERR_INVALID_ARG;
        }
        out->show_mask |= (uint8_t)(1u << bit);
    }

    out->text[0] = '\0';
    if (cJSON_GetObjectItemCaseSensitive(root, "text") && !get_text(root, "text", out->text, sizeof(out->text), 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    out->bg = (uint32_t)bg;
    out->fg = (uint32_t)fg;
    out->anchor = (proto_anchor_t)anchor;
    out->spinner = (proto_spinner_t)spinner;
    out->spinner_color = (uint32_t)spinner_color;
    out->offline_after_s = (uint32_t)offline_after_s;
    out->backlight = (uint32_t)backlight;
    return ESP_OK;
}

static esp_err_t parse_feed_card(const cJSON *item, proto_feed_card_t *out)
{
    char state[8] = "";
    if (!cJSON_IsObject(item) || !get_text(item, "id", out->id, sizeof(out->id), 1) ||
        !get_text(item, "label", out->label, sizeof(out->label), 0) ||
        !get_text(item, "value", out->value, sizeof(out->value), 0) ||
        !get_str(item, "state", state, sizeof(state), false)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->state = strcmp(state, "ok") == 0    ? PROTO_TILE_OK
                 : strcmp(state, "bad") == 0 ? PROTO_TILE_BAD
                                             : PROTO_TILE_UNKNOWN;
    return ESP_OK;
}

static esp_err_t parse_feed_note(const cJSON *item, proto_feed_note_t *out)
{
    static const char *const levels[] = {"info", "ok", "warn", "error"};
    double at;
    if (!cJSON_IsObject(item)) {
        return ESP_ERR_INVALID_ARG;
    }
    int level = get_name(item, "level", levels, sizeof(levels) / sizeof(levels[0]));
    if (level < 0 || !get_text(item, "id", out->id, sizeof(out->id), 1) ||
        !get_text(item, "title", out->title, sizeof(out->title), 1) ||
        !get_text(item, "body", out->body, sizeof(out->body), 0) ||
        !get_text(item, "src", out->src, sizeof(out->src), 0) || !get_whole(item, "at", 0, STATS_INT_MAX, &at)) {
        return ESP_ERR_INVALID_ARG;
    }
    out->level = (proto_level_t)level;
    out->at = (uint64_t)at;
    return ESP_OK;
}

static esp_err_t parse_feed(const cJSON *root, proto_feed_t *out)
{
    double sources;
    const cJSON *cards = cJSON_GetObjectItemCaseSensitive(root, "cards");
    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    if (!get_whole(root, "sources", 0, PROTO_FEED_SOURCES_MAX, &sources) || !cJSON_IsArray(cards) ||
        cJSON_GetArraySize(cards) > PROTO_FEED_CARDS_MAX || !cJSON_IsArray(notes) ||
        cJSON_GetArraySize(notes) > PROTO_FEED_NOTES_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    out->sources = (uint8_t)sources;

    const cJSON *item;
    out->n_cards = 0;
    cJSON_ArrayForEach(item, cards)
    {
        if (parse_feed_card(item, &out->cards[out->n_cards]) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        out->n_cards++;
    }
    out->n_notes = 0;
    cJSON_ArrayForEach(item, notes)
    {
        if (parse_feed_note(item, &out->notes[out->n_notes]) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        out->n_notes++;
    }
    return ESP_OK;
}

esp_err_t proto_parse_asset(const uint8_t *frame, size_t len, const uint8_t id[PROTO_ASSET_ID_LEN], uint32_t offset,
                            uint32_t asset_size, proto_asset_t *out)
{
    if (len < PROTO_ASSET_HEADER_LEN) {
        ESP_LOGW(TAG, "asset frame of %u bytes is shorter than its header", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(frame, "WDA1", 4) != 0) {
        ESP_LOGW(TAG, "binary frame is not an asset");
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(frame + 4, id, PROTO_ASSET_ID_LEN) != 0) {
        ESP_LOGW(TAG, "asset frame is for an id that was not asked for");
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t got =
        (uint32_t)frame[20] | (uint32_t)frame[21] << 8 | (uint32_t)frame[22] << 16 | (uint32_t)frame[23] << 24;
    if (got != offset) {
        ESP_LOGW(TAG, "asset frame is at offset %u, not the %u asked for", (unsigned)got, (unsigned)offset);
        return ESP_ERR_INVALID_ARG;
    }
    if (offset >= asset_size) {
        ESP_LOGW(TAG, "asked for offset %u of an asset of %u bytes", (unsigned)offset, (unsigned)asset_size);
        return ESP_ERR_INVALID_ARG;
    }
    // Every chunk is PROTO_ASSET_CHUNK bytes but the last, which is what is left.
    size_t want = asset_size - offset < PROTO_ASSET_CHUNK ? asset_size - offset : PROTO_ASSET_CHUNK;
    if (len != PROTO_ASSET_HEADER_LEN + want) {
        ESP_LOGW(TAG, "asset chunk at %u should be %u bytes, got %u", (unsigned)offset,
                 (unsigned)(PROTO_ASSET_HEADER_LEN + want), (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out->id, id, PROTO_ASSET_ID_LEN);
    out->offset = offset;
    out->len = want;
    out->data = frame + PROTO_ASSET_HEADER_LEN;
    return ESP_OK;
}

// True if the text has a \u0000 escape, which is only ever inside a string. cJSON would store it as a NUL that
// ends the string early, so a sleep or feed frame with one is refused up front. An escaped backslash (\\) is
// skipped, so \\u0000 is text and not an escape. Older frames keep their cut-at-the-NUL behavior.
static bool has_nul_escape(const char *json, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (json[i] != '\\') {
            continue;
        }
        if (json[i + 1] == '\\') {
            i++;
        } else if (json[i + 1] == 'u' && i + 6 <= len && memcmp(json + i + 2, "0000", 4) == 0) {
            return true;
        }
    }
    return false;
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
    } else if (strcmp(t, "stats") == 0) {
        out->type = PROTO_MSG_STATS;
        err = parse_stats(root, &out->stats);
    } else if (strcmp(t, "sleep") == 0) {
        out->type = PROTO_MSG_SLEEP;
        err = has_nul_escape(json, len) ? ESP_ERR_INVALID_ARG : parse_sleep(root, &out->sleep);
    } else if (strcmp(t, "feed") == 0) {
        out->type = PROTO_MSG_FEED;
        err = has_nul_escape(json, len) ? ESP_ERR_INVALID_ARG : parse_feed(root, &out->feed);
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
