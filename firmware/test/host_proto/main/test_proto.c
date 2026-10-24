// Host tests for proto.c. They run on the linux target, so no board is needed.
// Several cases pin current behaviour that the protocol spec does not promise;
// those are marked "pins".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_err.h"
#include "proto.h"
#include "psa/crypto.h"
#include "unity.h"

void test_grid_run(void);       // test_grid.c
void test_pageset_run(void);    // test_pageset.c
void test_hoststat_run(void);   // test_hoststat.c
void test_logring_run(void);    // test_logring.c
void test_diag_run(void);       // test_diag.c
void test_uimode_run(void);     // test_uimode.c
void test_sleepcfg_run(void);   // test_sleepcfg.c
void test_feedfmt_run(void);    // test_feedfmt.c
void test_cardfs_run(void);     // test_cardfs.c
void test_wifitxt_run(void);    // test_wifitxt.c
void test_svc_run(void);        // test_svc.c
void test_kbmap_run(void);      // test_kbmap.c
void test_wifistate_run(void);  // test_wifistate.c
void test_storagefmt_run(void); // test_storagefmt.c

static uint8_t frame[PROTO_MAX_ICON_FRAME];
static char json[8192];
static proto_msg_t msg;

// On the linux target, the static link leaves mbedtls's heap hooks unresolved once PSA is used
// (esp_mem.c sits in an archive that the linker has already passed). Supply them with libc here.
void *esp_mbedtls_mem_calloc(size_t n, size_t size)
{
    return calloc(n, size);
}

void esp_mbedtls_mem_free(void *ptr)
{
    free(ptr);
}

// A repeated-character string, for building long fields. Overwritten by the next call.
static const char *run_of(char c, size_t n)
{
    static char run[256];
    memset(run, c, n);
    run[n] = '\0';
    return run;
}

static void set_dims(uint8_t *f, uint16_t w, uint16_t h)
{
    f[20] = w & 0xff;
    f[21] = w >> 8;
    f[22] = h & 0xff;
    f[23] = h >> 8;
}

// Build a frame whose id matches its pixels, so only the case under test can fail.
static size_t make_icon(uint8_t *f, uint16_t w, uint16_t h)
{
    memcpy(f, "WDI1", 4);
    set_dims(f, w, h);
    size_t len = PROTO_ICON_HEADER_LEN + (size_t)w * h * 4;
    for (size_t i = PROTO_ICON_HEADER_LEN; i < len; i++) {
        f[i] = (uint8_t)(i * 7);
    }
    uint8_t hash[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t hash_len;
    TEST_ASSERT_EQUAL(PSA_SUCCESS, psa_hash_compute(PSA_ALG_SHA_256, f + 20, len - 20, hash, sizeof(hash), &hash_len));
    memcpy(f + 4, hash, PROTO_ICON_ID_LEN);
    return len;
}

// ---- Icon frames ----

void test_icon_shorter_than_its_header_is_refused(void)
{
    make_icon(frame, 1, 1);
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, 0, &icon));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, 23, &icon));
    // A bare header passes the magic check and fails the length check, since a 1x1 frame needs 28 bytes.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, 24, &icon));
}

void test_icon_bad_magic_is_refused(void)
{
    size_t len = make_icon(frame, 1, 1);
    frame[3] = '2';
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse_icon(frame, len, &icon));
}

void test_icon_side_outside_1_to_256_is_refused(void)
{
    size_t len = make_icon(frame, 1, 1);
    proto_icon_t icon;
    set_dims(frame, 0, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len, &icon));
    set_dims(frame, 257, 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len, &icon));
    set_dims(frame, 1, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len, &icon));
    set_dims(frame, 1, 257);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len, &icon));
}

void test_icon_length_one_byte_off_is_refused(void)
{
    size_t len = make_icon(frame, 4, 4);
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len + 1, &icon));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_icon(frame, len - 1, &icon));
}

void test_icon_flipped_pixel_fails_the_id_check(void)
{
    size_t len = make_icon(frame, 2, 2);
    frame[PROTO_ICON_HEADER_LEN + 5] ^= 0x01;
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, proto_parse_icon(frame, len, &icon));
}

void test_icon_valid_1x1_is_accepted(void)
{
    size_t len = make_icon(frame, 1, 1);
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse_icon(frame, len, &icon));
    TEST_ASSERT_EQUAL_UINT16(1, icon.w);
    TEST_ASSERT_EQUAL_UINT16(1, icon.h);
    TEST_ASSERT_EQUAL_PTR(frame + PROTO_ICON_HEADER_LEN, icon.pixels);
    TEST_ASSERT_EQUAL_MEMORY(frame + 4, icon.id, PROTO_ICON_ID_LEN);
}

void test_icon_valid_256x256_is_accepted(void)
{
    size_t len = make_icon(frame, 256, 256);
    TEST_ASSERT_EQUAL_size_t(PROTO_MAX_ICON_FRAME, len);
    proto_icon_t icon;
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse_icon(frame, len, &icon));
    TEST_ASSERT_EQUAL_UINT16(256, icon.w);
    TEST_ASSERT_EQUAL_UINT16(256, icon.h);
}

// ---- Tile messages ----

void test_tile_btn_missing_is_refused(void)
{
    const char *s = "{\"t\":\"tile\",\"text\":\"x\"}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(s, strlen(s), &msg));
}

void test_tile_btn_of_63_is_accepted_and_64_is_refused(void)
{
    snprintf(json, sizeof(json), "{\"t\":\"tile\",\"btn\":\"%s\",\"text\":\"\"}", run_of('b', 63));
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_size_t(63, strlen(msg.tile.btn));

    snprintf(json, sizeof(json), "{\"t\":\"tile\",\"btn\":\"%s\",\"text\":\"\"}", run_of('b', 64));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_tile_text_as_a_number_is_refused(void)
{
    const char *s = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":5}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(s, strlen(s), &msg));
}

void test_tile_text_longer_than_64_is_cut_to_64(void)
{
    snprintf(json, sizeof(json), "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"%s\"}", run_of('x', 100));
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_size_t(64, strlen(msg.tile.text));
}

void test_tile_empty_text_is_accepted(void)
{
    const char *s = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL_STRING("", msg.tile.text);
}

void test_tile_state_absent_weird_or_a_number_is_unknown(void)
{
    const char *ok = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"\",\"state\":\"ok\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(ok, strlen(ok), &msg));
    TEST_ASSERT_EQUAL(PROTO_TILE_OK, msg.tile.state);

    const char *absent = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"\"}";
    const char *weird = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"\",\"state\":\"weird\"}";
    const char *number = "{\"t\":\"tile\",\"btn\":\"b\",\"text\":\"\",\"state\":1}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(absent, strlen(absent), &msg));
    TEST_ASSERT_EQUAL(PROTO_TILE_UNKNOWN, msg.tile.state);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(weird, strlen(weird), &msg));
    TEST_ASSERT_EQUAL(PROTO_TILE_UNKNOWN, msg.tile.state);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(number, strlen(number), &msg));
    TEST_ASSERT_EQUAL(PROTO_TILE_UNKNOWN, msg.tile.state);
}

// pins: "\u0000" ends the C string, so the btn is cut at the NUL and the rest is dropped.
void test_tile_btn_with_a_nul_escape_is_cut_at_the_nul(void)
{
    const char *s = "{\"t\":\"tile\",\"btn\":\"a\\u0000x\",\"text\":\"\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL_STRING("a", msg.tile.btn);
}

// ---- Report builder and ack parser ----

static const char ELF64[] = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";

static proto_report_t crash_report(void)
{
    proto_report_t r = {.kind = PROTO_REPORT_CRASH, .pc = 0x4ff01234, .ra = 0x5, .addr = 0, .same_build = true};
    strcpy(r.id, "a1b2c3d4");
    strcpy(r.reason, "panic");
    strcpy(r.task, "link");
    strcpy(r.cause, "LoadAccessFault");
    strcpy(r.elf, ELF64);
    return r;
}

static void assert_str_field(const cJSON *root, const char *key, const char *want)
{
    const cJSON *f = cJSON_GetObjectItemCaseSensitive(root, key);
    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(f), key);
    TEST_ASSERT_EQUAL_STRING(want, f->valuestring);
}

void test_report_crash_has_every_field_with_the_right_type(void)
{
    proto_report_t r = crash_report();
    size_t len = proto_build_report(json, sizeof(json), &r);
    TEST_ASSERT_GREATER_THAN(0, len);
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_str_field(root, "t", "report");
    assert_str_field(root, "id", "a1b2c3d4");
    assert_str_field(root, "kind", "crash");
    assert_str_field(root, "reason", "panic");
    assert_str_field(root, "task", "link");
    assert_str_field(root, "pc", "0x4ff01234");
    assert_str_field(root, "ra", "0x00000005");
    assert_str_field(root, "cause", "LoadAccessFault");
    assert_str_field(root, "addr", "0x00000000");
    assert_str_field(root, "elf", ELF64);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "same_build")));
    TEST_ASSERT_EQUAL_INT(11, cJSON_GetArraySize(root));
    cJSON_Delete(root);
}

void test_report_reset_sends_only_id_kind_and_reason(void)
{
    proto_report_t r = {.kind = PROTO_REPORT_RESET};
    strcpy(r.id, "00ff00ff");
    strcpy(r.reason, "brownout");
    TEST_ASSERT_GREATER_THAN(0, proto_build_report(json, sizeof(json), &r));
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    assert_str_field(root, "kind", "reset");
    assert_str_field(root, "reason", "brownout");
    TEST_ASSERT_EQUAL_INT(4, cJSON_GetArraySize(root));
    cJSON_Delete(root);
}

void test_report_invalid_fields_are_refused(void)
{
    proto_report_t r = crash_report();
    strcpy(r.id, "A1b2c3d4"); // uppercase
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
    r = crash_report();
    strcpy(r.reason, "oops");
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
    r = crash_report();
    r.elf[63] = 'G';
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
    r = crash_report();
    r.elf[10] = 'A'; // uppercase
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
    r = crash_report();
    strcpy(r.task, "bad\ntask");
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
    r = crash_report();
    r.cause[0] = '\0';
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
}

void test_report_task_of_16_fits_and_17_does_not(void)
{
    proto_report_t r = crash_report();
    memcpy(r.task, run_of('t', 16), 17);
    TEST_ASSERT_GREATER_THAN(0, proto_build_report(json, sizeof(json), &r));
    memcpy(r.task, run_of('t', 17), 18);
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, sizeof(json), &r));
}

void test_report_too_small_a_buffer_returns_zero(void)
{
    proto_report_t r = crash_report();
    TEST_ASSERT_EQUAL_size_t(0, proto_build_report(json, 40, &r));
}

void test_layout_ack_carries_the_rev(void)
{
    size_t len = proto_build_layout_ack(json, sizeof(json), 7);
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"layout_ack\",\"rev\":7}", json);
    TEST_ASSERT_EQUAL_size_t(strlen(json), len);
    TEST_ASSERT_GREATER_THAN(0, proto_build_layout_ack(json, sizeof(json), UINT32_MAX));
    TEST_ASSERT_EQUAL_STRING("{\"t\":\"layout_ack\",\"rev\":4294967295}", json);
    TEST_ASSERT_EQUAL_size_t(0, proto_build_layout_ack(json, 10, 7));
}

void test_report_ack_valid_id_is_parsed(void)
{
    const char *s = "{\"t\":\"report_ack\",\"id\":\"a1b2c3d4\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_REPORT_ACK, msg.type);
    TEST_ASSERT_EQUAL_STRING("a1b2c3d4", msg.report_ack_id);
}

void test_report_ack_bad_id_is_refused(void)
{
    const char *bad[] = {
        "{\"t\":\"report_ack\"}",
        "{\"t\":\"report_ack\",\"id\":5}",
        "{\"t\":\"report_ack\",\"id\":\"a1b2c3d\"}",
        "{\"t\":\"report_ack\",\"id\":\"a1b2c3d45\"}",
        "{\"t\":\"report_ack\",\"id\":\"A1B2C3D4\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, proto_parse(bad[i], strlen(bad[i]), &msg), bad[i]);
    }
}

// ---- General message handling ----

void test_general_unknown_t_is_parsed_as_unknown(void)
{
    const char *s = "{\"t\":\"zzz\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_UNKNOWN, msg.type);
    TEST_ASSERT_EQUAL_STRING("zzz", msg.unknown_t);
}

void test_general_t_that_is_not_a_string_is_refused(void)
{
    const char *s = "{\"t\":5}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(s, strlen(s), &msg));
}

void test_general_root_that_is_not_an_object_is_refused(void)
{
    const char *s = "[1]";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(s, strlen(s), &msg));
}

void test_general_truncated_json_is_refused(void)
{
    const char *s = "{\"t\":\"welcome\"";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(s, strlen(s), &msg));
}

// pins: cJSON stops at the end of the first value, so bytes after it are ignored.
void test_general_trailing_garbage_is_accepted(void)
{
    const char *s = "{\"t\":\"welcome\"}garbage";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_WELCOME, msg.type);
}

// pins: only the first object is read, so the second one is ignored.
void test_general_two_objects_back_to_back_read_the_first(void)
{
    const char *s = "{\"t\":\"welcome\",\"name\":\"a\"}{\"t\":\"error\",\"code\":\"x\"}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(s, strlen(s), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_WELCOME, msg.type);
    TEST_ASSERT_EQUAL_STRING("a", msg.welcome_name);
}

// ---- Layout messages ----

// The fields every layout test shares. A test adds the one field it varies.
#define BUTTON "\"id\":\"b1\",\"label\":\"L\",\"col\":0,\"row\":0,\"bg\":0,\"fg\":0"

// A 4x2 layout holding the given button objects, which are passed in as one JSON fragment.
static void layout_with(const char *buttons)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":1,\"cols\":4,\"rows\":2,\"buttons\":[%s]}", buttons);
}

static void layout_with_grid(const char *grid_fields)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":1,%s,\"buttons\":[]}", grid_fields);
}

void test_layout_cols_outside_1_to_8_is_refused(void)
{
    layout_with_grid("\"cols\":0,\"rows\":1");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
    layout_with_grid("\"cols\":9,\"rows\":1");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_rows_of_6_is_refused(void)
{
    layout_with_grid("\"cols\":4,\"rows\":6");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_40_buttons_is_accepted_and_41_is_refused(void)
{
    // Each button is a fixed-width object, so the count alone decides the outcome.
    char buttons[4096];
    for (int n = 40; n <= 41; n++) {
        size_t len = 0;
        for (int i = 0; i < n; i++) {
            len += (size_t)snprintf(buttons + len, sizeof(buttons) - len,
                                    "%s{\"id\":\"b%d\",\"label\":\"\",\"col\":0,\"row\":0,\"bg\":0,\"fg\":0}",
                                    i ? "," : "", i);
        }
        snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":1,\"cols\":8,\"rows\":5,\"buttons\":[%s]}", buttons);
        esp_err_t want = n == 40 ? ESP_OK : ESP_ERR_INVALID_ARG;
        TEST_ASSERT_EQUAL(want, proto_parse(json, strlen(json), &msg));
    }
}

void test_layout_col_equal_to_cols_is_refused(void)
{
    layout_with("{\"id\":\"b1\",\"label\":\"L\",\"col\":4,\"row\":0,\"bg\":0,\"fg\":0}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_icon_null_is_refused(void)
{
    layout_with("{" BUTTON ",\"icon\":null}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_icon_must_be_32_lowercase_hex(void)
{
    layout_with("{" BUTTON ",\"icon\":\"0123456789abcdef0123456789abcdef\"}");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_TRUE(msg.layout.buttons[0].has_icon);

    layout_with("{" BUTTON ",\"icon\":\"0123456789abcdef0123456789abcde\"}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));

    layout_with("{" BUTTON ",\"icon\":\"0123456789ABCDEF0123456789ABCDEF\"}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_tile_as_a_string_is_refused(void)
{
    layout_with("{" BUTTON ",\"tile\":\"yes\"}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

void test_layout_rev_of_1e400_or_minus_1_is_refused(void)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":1e400,\"cols\":4,\"rows\":2,\"buttons\":[]}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":-1,\"cols\":4,\"rows\":2,\"buttons\":[]}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));
}

// pins: a fraction is cut toward zero, not refused.
void test_layout_fractions_are_truncated(void)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":7.9,\"cols\":4,\"rows\":2,\"buttons\":[]}");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_UINT32(7, msg.layout.rev);

    layout_with("{\"id\":\"b1\",\"label\":\"L\",\"col\":1.9,\"row\":0,\"bg\":0,\"fg\":0}");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_UINT8(1, msg.layout.buttons[0].col);
}

// cJSON counts the root object as one level, so 8 nested arrays under it make 9 containers in all.
void test_layout_nesting_depth_9_is_refused(void)
{
    snprintf(json, sizeof(json),
             "{\"t\":\"layout\",\"rev\":1,\"cols\":4,\"rows\":2,\"buttons\":[],"
             "\"x\":[[[[[[[[0]]]]]]]]}");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse(json, strlen(json), &msg));

    // Control: one level less is still under the limit of 8, so the refusal above is the limit.
    snprintf(json, sizeof(json),
             "{\"t\":\"layout\",\"rev\":1,\"cols\":4,\"rows\":2,\"buttons\":[],"
             "\"x\":[[[[[[[0]]]]]]]}");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
}

// pins F2: strlcpy cuts at 47 bytes, which splits the two-byte "é" after its lead byte.
void test_layout_label_cut_inside_a_utf8_sequence_leaves_a_lone_lead_byte(void)
{
    char label[50];
    memset(label, 'A', 46);
    label[46] = (char)0xC3;
    label[47] = (char)0xA9;
    label[48] = '\0';
    char button[256];
    snprintf(button, sizeof(button), "{\"id\":\"b1\",\"label\":\"%s\",\"col\":0,\"row\":0,\"bg\":0,\"fg\":0}", label);
    layout_with(button);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_size_t(47, strlen(msg.layout.buttons[0].label));
    TEST_ASSERT_EQUAL_UINT8(0xC3, (uint8_t)msg.layout.buttons[0].label[46]);
    TEST_ASSERT_EQUAL_UINT8(0, (uint8_t)msg.layout.buttons[0].label[47]);
}

// ---- Fonts ----

// A layout whose message carries `font_field` (a JSON fragment such as "\"font\":\"inter\"", or "") and one button.
static void layout_font(const char *font_field, const char *button_fields)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":1,\"cols\":4,\"rows\":2,%s\"buttons\":[{" BUTTON "%s}]}",
             font_field, button_fields);
}

void test_font_none_given_is_montserrat_without_a_flag(void)
{
    layout_font("", ",\"tile\":true");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.font);
    TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].value_font);
    TEST_ASSERT_FALSE(msg.layout.unknown_font);
}

void test_font_on_the_message_covers_every_label_and_value(void)
{
    layout_font("\"font\":\"inter\",", ",\"tile\":true");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.font);
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.buttons[0].value_font);
    TEST_ASSERT_FALSE(msg.layout.unknown_font);
}

void test_font_label_font_carries_over_to_the_value_unless_value_font_is_given(void)
{
    layout_font("\"font\":\"inter\",", ",\"tile\":true,\"label_font\":\"orbitron\"");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_orbitron, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_orbitron, msg.layout.buttons[0].value_font);

    layout_font("\"font\":\"inter\",", ",\"tile\":true,\"label_font\":\"orbitron\",\"value_font\":\"fira-code\"");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_orbitron, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_fira_code, msg.layout.buttons[0].value_font);

    // A value_font alone leaves the label on the message font.
    layout_font("\"font\":\"inter\",", ",\"tile\":true,\"value_font\":\"oxanium\"");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_oxanium, msg.layout.buttons[0].value_font);
}

void test_font_every_id_is_known(void)
{
    static const char *const names[WD_FONT_COUNT] = {"montserrat",  "inter",    "roboto", "fira-code",
                                                     "roboto-mono", "orbitron", "oxanium"};
    for (int i = 0; i < WD_FONT_COUNT; i++) {
        bool known = false;
        TEST_ASSERT_EQUAL(i, wd_font_from_name(names[i], &known));
        TEST_ASSERT_TRUE(known);
    }
}

// A font is cosmetic: a bad value is Montserrat with the flag set, and the layout is still accepted.
void test_font_unknown_or_mistyped_values_fall_back_to_montserrat_and_set_the_flag(void)
{
    char long_name[260];
    snprintf(long_name, sizeof(long_name), "\"label_font\":\"%s\"", run_of('a', 200));
    const char *bad_labels[] = {"\"label_font\":\"nope\"", "\"label_font\":7", "\"label_font\":null",
                                "\"label_font\":{}",       long_name,          "\"label_font\":\"Inter\"",
                                "\"label_font\":\"\""};
    for (size_t i = 0; i < sizeof(bad_labels) / sizeof(bad_labels[0]); i++) {
        char fields[300];
        snprintf(fields, sizeof(fields), ",\"tile\":true,%s", bad_labels[i]);
        layout_font("\"font\":\"inter\",", fields);
        TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
        TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.font);
        TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].label_font);
        TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].value_font);
        TEST_ASSERT_TRUE(msg.layout.unknown_font);
    }

    snprintf(long_name, sizeof(long_name), "\"font\":\"%s\",", run_of('a', 200));
    const char *bad_messages[] = {"\"font\":\"nope\",", "\"font\":7,", "\"font\":null,", "\"font\":{},", long_name};
    for (size_t i = 0; i < sizeof(bad_messages) / sizeof(bad_messages[0]); i++) {
        layout_font(bad_messages[i], ",\"tile\":true");
        TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
        TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.font);
        TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].label_font);
        TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].value_font);
        TEST_ASSERT_TRUE(msg.layout.unknown_font);
    }

    // A bad value_font leaves the label alone.
    layout_font("\"font\":\"inter\",", ",\"tile\":true,\"value_font\":\"nope\"");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_MONTSERRAT, msg.layout.buttons[0].value_font);
    TEST_ASSERT_TRUE(msg.layout.unknown_font);
}

// The flag belongs to one message: a clean layout after a bad one clears it.
void test_font_flag_does_not_stick_to_the_next_layout(void)
{
    layout_font("\"font\":\"nope\",", "");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_TRUE(msg.layout.unknown_font);
    layout_font("", "");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_FALSE(msg.layout.unknown_font);
}

// The fullest layout: 40 buttons with 63-byte ids, labels and every font field at its longest.
void test_font_fullest_layout_with_every_font_field_parses(void)
{
#define FILLER "iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii"
    static char big[PROTO_MAX_MSG];
    size_t len = (size_t)snprintf(
        big, sizeof(big), "{\"t\":\"layout\",\"rev\":1,\"cols\":8,\"rows\":5,\"font\":\"roboto-mono\",\"buttons\":[");
    for (int i = 0; i < 40; i++) {
        len += (size_t)snprintf(
            big + len, sizeof(big) - len,
            "%s{\"id\":\"%.60s%03d\",\"label\":\"%.47s\",\"col\":%d,\"row\":%d,\"bg\":16777215,"
            "\"fg\":16777215,\"tile\":true,\"label_font\":\"roboto-mono\",\"value_font\":\"roboto-mono\"}",
            i ? "," : "", FILLER, i, FILLER, i % 8, i / 8);
    }
    len += (size_t)snprintf(big + len, sizeof(big) - len, "]}");
    TEST_ASSERT_LESS_THAN(PROTO_MAX_MSG, len);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(big, len, &msg));
    TEST_ASSERT_EQUAL_size_t(40, msg.layout.n_buttons);
    TEST_ASSERT_EQUAL(WD_FONT_roboto_mono, msg.layout.buttons[39].value_font);
    TEST_ASSERT_FALSE(msg.layout.unknown_font);
}

// ---- Layout pages ----

// A one-button 4x2 layout whose message also carries `page_fields` (a JSON fragment ending in a comma, or "").
static esp_err_t parse_page(const char *page_fields)
{
    snprintf(json, sizeof(json), "{\"t\":\"layout\",\"rev\":7,%s\"cols\":4,\"rows\":2,\"buttons\":[{" BUTTON "}]}",
             page_fields);
    return proto_parse(json, strlen(json), &msg);
}

void test_page_none_given_is_page_0_of_1_with_an_empty_id(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_page(""));
    TEST_ASSERT_EQUAL_UINT8(0, msg.layout.page);
    TEST_ASSERT_EQUAL_UINT8(1, msg.layout.pages);
    TEST_ASSERT_EQUAL_STRING("", msg.layout.page_id);
    TEST_ASSERT_EQUAL_UINT32(7, msg.layout.rev);
    TEST_ASSERT_EQUAL_size_t(1, msg.layout.n_buttons);
}

// The message buffer is reused, so a plain layout after a paged one must reset the page fields.
void test_plain_layout_after_a_paged_one_resets_to_page_0_of_1(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_page("\"page\":2,\"pages\":3,\"page_id\":\"media\","));
    TEST_ASSERT_EQUAL(ESP_OK, parse_page(""));
    TEST_ASSERT_EQUAL_UINT8(0, msg.layout.page);
    TEST_ASSERT_EQUAL_UINT8(1, msg.layout.pages);
    TEST_ASSERT_EQUAL_STRING("", msg.layout.page_id);
}

void test_page_3_of_4_is_accepted(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_page("\"page\":3,\"pages\":4,\"page_id\":\"media\","));
    TEST_ASSERT_EQUAL_UINT8(3, msg.layout.page);
    TEST_ASSERT_EQUAL_UINT8(4, msg.layout.pages);
    TEST_ASSERT_EQUAL_STRING("media", msg.layout.page_id);
    TEST_ASSERT_EQUAL_size_t(1, msg.layout.n_buttons);
}

void test_page_0_of_1_is_accepted(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_page("\"page\":0,\"pages\":1,\"page_id\":\"main\","));
    TEST_ASSERT_EQUAL_UINT8(0, msg.layout.page);
    TEST_ASSERT_EQUAL_UINT8(1, msg.layout.pages);
    TEST_ASSERT_EQUAL_STRING("main", msg.layout.page_id);
}

void test_page_equal_to_pages_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":3,\"pages\":3,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":4,\"pages\":4,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":-1,\"pages\":4,\"page_id\":\"x\","));
}

void test_pages_of_0_or_5_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":0,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":5,\"page_id\":\"x\","));
}

void test_page_without_pages_or_without_page_id_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":2,"));
}

void test_pages_or_page_id_without_page_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"pages\":2,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"pages\":2,"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page_id\":\"x\","));
}

void test_page_id_must_be_1_to_63_bytes(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":2,\"page_id\":\"\","));

    char fields[128];
    snprintf(fields, sizeof(fields), "\"page\":0,\"pages\":2,\"page_id\":\"%s\",", run_of('i', 63));
    TEST_ASSERT_EQUAL(ESP_OK, parse_page(fields));
    TEST_ASSERT_EQUAL_size_t(63, strlen(msg.layout.page_id));

    snprintf(fields, sizeof(fields), "\"page\":0,\"pages\":2,\"page_id\":\"%s\",", run_of('i', 64));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page(fields));
}

void test_page_fields_of_the_wrong_type_are_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":\"0\",\"pages\":2,\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":\"2\",\"page_id\":\"x\","));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":2,\"page_id\":7,"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_page("\"page\":0,\"pages\":2,\"page_id\":null,"));
}

// A page message resolves font and the per-button font fields exactly as a single layout does.
void test_page_message_resolves_fonts_like_a_single_layout(void)
{
    snprintf(json, sizeof(json),
             "{\"t\":\"layout\",\"rev\":7,\"page\":1,\"pages\":3,\"page_id\":\"media\",\"cols\":4,\"rows\":2,"
             "\"font\":\"inter\",\"buttons\":[{" BUTTON ",\"tile\":true,\"label_font\":\"orbitron\"}]}");
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(json, strlen(json), &msg));
    TEST_ASSERT_EQUAL_UINT8(1, msg.layout.page);
    TEST_ASSERT_EQUAL(WD_FONT_inter, msg.layout.font);
    TEST_ASSERT_EQUAL(WD_FONT_orbitron, msg.layout.buttons[0].label_font);
    TEST_ASSERT_EQUAL(WD_FONT_orbitron, msg.layout.buttons[0].value_font);
    TEST_ASSERT_FALSE(msg.layout.unknown_font);
}

// ---- Hello ----

static const uint8_t TOKEN[PROTO_TOKEN_LEN] = {0};

void test_hello_without_caps_has_no_caps_key(void)
{
    char buf[256];
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, "0.3.0", 1024, 600, NULL, 0);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NULL(strstr(buf, "caps"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"screen\":[1024,600]"));
}

void test_hello_with_fonts_names_it_in_caps(void)
{
    char buf[256];
    const char *const caps[] = {"fonts"};
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, "0.3.0", 1024, 600, caps, 1);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"caps\":[\"fonts\"]"));
}

void test_hello_with_two_caps_and_a_32_character_fw_fits_256_bytes(void)
{
    char buf[256];
    const char *const caps[] = {"fonts", "pages"};
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, run_of('v', 32), 1024, 600, caps, 2);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_LESS_THAN(sizeof(buf), len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"caps\":[\"fonts\",\"pages\"]"));
}

void test_hello_with_three_caps_and_a_32_character_fw_is_219_bytes(void)
{
    char buf[256];
    const char *const caps[] = {"fonts", "pages", "status_bar"};
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, run_of('v', 32), 1024, 600, caps, 3);
    TEST_ASSERT_EQUAL_size_t(219, len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"caps\":[\"fonts\",\"pages\",\"status_bar\"]"));
}

// ---- Stats ----

// The fixtures in app/src-tauri/tests/fixtures/, which the daemon round-trips.
static const char STATS_FULL[] =
    "{\"t\":\"stats\",\"time\":1792692433,\"utc_offset\":-14400,\"cpu\":12,\"cpu_temp\":64,\"gpu\":23,"
    "\"gpu_temp\":41,\"mem_used\":10522394624,\"mem_total\":33395638272,\"net_rx\":125000,\"net_tx\":34000,"
    "\"temp_unit\":\"c\",\"net_unit\":\"si\",\"colors\":{\"cpu\":16757504}}";
static const char STATS_COLORS[] =
    "{\"t\":\"stats\",\"time\":1792692434,\"utc_offset\":3600,\"cpu\":97,\"cpu_temp\":88,\"gpu\":100,"
    "\"gpu_temp\":84,\"mem_used\":91000000000,\"mem_total\":98495209472,\"net_rx\":125000000,"
    "\"net_tx\":34000000,\"temp_unit\":\"f\",\"net_unit\":\"bits\",\"colors\":{\"cpu\":16757504,"
    "\"mem\":15022389,\"gpu\":2001125,\"cpu_temp\":15022389,\"gpu_temp\":2001125,\"net_rx\":4431943,"
    "\"net_tx\":16757504}}";
static const char STATS_MIN[] = "{\"t\":\"stats\",\"time\":1792692433}";

// Parse a stats message whose body is `fields`, the members after "t".
static esp_err_t stats_with(const char *fields)
{
    snprintf(json, sizeof(json), "{\"t\":\"stats\",%s}", fields);
    return proto_parse(json, strlen(json), &msg);
}

void test_stats_full_fixture_has_every_field(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(STATS_FULL, strlen(STATS_FULL), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_STATS, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1792692433ULL, msg.stats.time);
    TEST_ASSERT_TRUE(msg.stats.has_utc_offset);
    TEST_ASSERT_EQUAL_INT32(-14400, msg.stats.utc_offset);
    TEST_ASSERT_TRUE(msg.stats.has_cpu);
    TEST_ASSERT_EQUAL_UINT8(12, msg.stats.cpu);
    TEST_ASSERT_TRUE(msg.stats.has_mem);
    TEST_ASSERT_EQUAL_UINT64(10522394624ULL, msg.stats.mem_used);
    TEST_ASSERT_EQUAL_UINT64(33395638272ULL, msg.stats.mem_total);
    TEST_ASSERT_TRUE(msg.stats.has_net);
    TEST_ASSERT_EQUAL_UINT64(125000, msg.stats.net_rx);
    TEST_ASSERT_EQUAL_UINT64(34000, msg.stats.net_tx);
    TEST_ASSERT_TRUE(msg.stats.has_gpu);
    TEST_ASSERT_EQUAL_UINT8(23, msg.stats.gpu);
    TEST_ASSERT_TRUE(msg.stats.has_cpu_temp);
    TEST_ASSERT_EQUAL_INT16(64, msg.stats.cpu_temp);
    TEST_ASSERT_TRUE(msg.stats.has_gpu_temp);
    TEST_ASSERT_EQUAL_INT16(41, msg.stats.gpu_temp);
    TEST_ASSERT_EQUAL(PROTO_TEMP_C, msg.stats.temp_unit);
    TEST_ASSERT_EQUAL(PROTO_NET_SI, msg.stats.net_unit);
    TEST_ASSERT_EQUAL_UINT8(1u << PROTO_COLOR_CPU, msg.stats.colors_mask);
    TEST_ASSERT_EQUAL_UINT32(16757504u, msg.stats.colors[PROTO_COLOR_CPU]);
}

void test_stats_colors_fixture_has_every_unit_and_color(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(STATS_COLORS, strlen(STATS_COLORS), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_STATS, msg.type);
    TEST_ASSERT_EQUAL(PROTO_TEMP_F, msg.stats.temp_unit);
    TEST_ASSERT_EQUAL(PROTO_NET_BITS, msg.stats.net_unit);
    TEST_ASSERT_EQUAL_UINT8(100, msg.stats.gpu);
    TEST_ASSERT_EQUAL_INT16(88, msg.stats.cpu_temp);
    TEST_ASSERT_EQUAL_INT16(84, msg.stats.gpu_temp);
    TEST_ASSERT_EQUAL_UINT8(0x7F, msg.stats.colors_mask);
    TEST_ASSERT_EQUAL_UINT32(16757504u, msg.stats.colors[PROTO_COLOR_CPU]);
    TEST_ASSERT_EQUAL_UINT32(15022389u, msg.stats.colors[PROTO_COLOR_MEM]);
    TEST_ASSERT_EQUAL_UINT32(2001125u, msg.stats.colors[PROTO_COLOR_GPU]);
    TEST_ASSERT_EQUAL_UINT32(15022389u, msg.stats.colors[PROTO_COLOR_CPU_TEMP]);
    TEST_ASSERT_EQUAL_UINT32(2001125u, msg.stats.colors[PROTO_COLOR_GPU_TEMP]);
    TEST_ASSERT_EQUAL_UINT32(4431943u, msg.stats.colors[PROTO_COLOR_NET_RX]);
    TEST_ASSERT_EQUAL_UINT32(16757504u, msg.stats.colors[PROTO_COLOR_NET_TX]);
}

void test_stats_time_only_leaves_every_optional_field_out(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(STATS_MIN, strlen(STATS_MIN), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_STATS, msg.type);
    TEST_ASSERT_EQUAL_UINT64(1792692433ULL, msg.stats.time);
    TEST_ASSERT_FALSE(msg.stats.has_utc_offset);
    TEST_ASSERT_FALSE(msg.stats.has_cpu);
    TEST_ASSERT_FALSE(msg.stats.has_mem);
    TEST_ASSERT_FALSE(msg.stats.has_net);
    TEST_ASSERT_FALSE(msg.stats.has_gpu);
    TEST_ASSERT_FALSE(msg.stats.has_cpu_temp);
    TEST_ASSERT_FALSE(msg.stats.has_gpu_temp);
    TEST_ASSERT_EQUAL(PROTO_TEMP_C, msg.stats.temp_unit);
    TEST_ASSERT_EQUAL(PROTO_NET_SI, msg.stats.net_unit);
    TEST_ASSERT_EQUAL_UINT8(0, msg.stats.colors_mask);
}

void test_stats_time_absent_or_null_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"cpu\":5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":null"));
}

void test_stats_time_as_a_string_negative_or_2_to_the_53_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":\"1792692433\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":-1"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":9007199254740992"));
}

void test_stats_time_of_2_to_the_53_minus_1_is_accepted(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":9007199254740991"));
    TEST_ASSERT_EQUAL_UINT64(9007199254740991ULL, msg.stats.time);
}

void test_stats_cpu_0_and_100_are_accepted_and_101_and_minus_1_are_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"cpu\":0"));
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"cpu\":100"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu\":101"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu\":-1"));
}

void test_stats_utc_offset_outside_plus_minus_64800_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"utc_offset\":64800"));
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"utc_offset\":-64800"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"utc_offset\":64801"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"utc_offset\":-64801"));
}

void test_stats_mem_used_without_mem_total_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"mem_used\":5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"mem_total\":5"));
}

void test_stats_mem_used_above_mem_total_is_refused_and_equal_is_accepted(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"mem_used\":6,\"mem_total\":5"));
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"mem_used\":5,\"mem_total\":5"));
}

void test_stats_net_rx_without_net_tx_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_rx\":5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_tx\":5"));
}

void test_stats_fraction_in_an_int_field_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1.5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu\":12.5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_rx\":5.5,\"net_tx\":1"));
}

void test_stats_unknown_fields_are_ignored(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":5,\"extra\":[1,{\"x\":2}],\"cpu_avg\":3"));
    TEST_ASSERT_EQUAL_UINT64(5, msg.stats.time);
    TEST_ASSERT_FALSE(msg.stats.has_cpu);
}

void test_stats_gpu_0_and_100_accepted_and_101_minus_1_or_a_fraction_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu\":0"));
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu\":100"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu\":101"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu\":-1"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu\":23.5"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu\":null"));
}

void test_stats_cpu_temp_minus_40_to_150_accepted_and_outside_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"cpu_temp\":-40"));
    TEST_ASSERT_EQUAL_INT16(-40, msg.stats.cpu_temp);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"cpu_temp\":150"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu_temp\":-41"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu_temp\":151"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"cpu_temp\":64.5"));
}

void test_stats_gpu_temp_minus_40_to_150_accepted_and_outside_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu_temp\":-40"));
    TEST_ASSERT_EQUAL_INT16(-40, msg.stats.gpu_temp);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu_temp\":150"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu_temp\":-41"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"gpu_temp\":151"));
}

void test_stats_gpu_and_its_temperatures_are_independent(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu_temp\":41"));
    TEST_ASSERT_FALSE(msg.stats.has_gpu);
    TEST_ASSERT_TRUE(msg.stats.has_gpu_temp);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"gpu\":23"));
    TEST_ASSERT_TRUE(msg.stats.has_gpu);
    TEST_ASSERT_FALSE(msg.stats.has_gpu_temp);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"cpu_temp\":64"));
    TEST_ASSERT_FALSE(msg.stats.has_gpu);
    TEST_ASSERT_TRUE(msg.stats.has_cpu_temp);
}

void test_stats_temp_unit_accepts_only_c_and_f(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"temp_unit\":\"c\""));
    TEST_ASSERT_EQUAL(PROTO_TEMP_C, msg.stats.temp_unit);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"temp_unit\":\"f\""));
    TEST_ASSERT_EQUAL(PROTO_TEMP_F, msg.stats.temp_unit);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"temp_unit\":\"F\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"temp_unit\":\"k\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"temp_unit\":1"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"temp_unit\":null"));
}

void test_stats_net_unit_accepts_si_binary_and_bits_only(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"net_unit\":\"si\""));
    TEST_ASSERT_EQUAL(PROTO_NET_SI, msg.stats.net_unit);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"net_unit\":\"binary\""));
    TEST_ASSERT_EQUAL(PROTO_NET_BINARY, msg.stats.net_unit);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"net_unit\":\"bits\""));
    TEST_ASSERT_EQUAL(PROTO_NET_BITS, msg.stats.net_unit);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_unit\":\"mib\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_unit\":\"binaryx\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"net_unit\":2"));
}

void test_stats_colors_as_an_object_of_whole_rgb_values(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"colors\":{}"));
    TEST_ASSERT_EQUAL_UINT8(0, msg.stats.colors_mask);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"colors\":{\"extra\":5}"));
    TEST_ASSERT_EQUAL_UINT8(0, msg.stats.colors_mask);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"colors\":{\"cpu\":16777215}"));
    TEST_ASSERT_EQUAL_UINT32(16777215u, msg.stats.colors[PROTO_COLOR_CPU]);
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"colors\":{\"cpu\":0,\"gpu_temp\":5}"));
    TEST_ASSERT_EQUAL_UINT8((1u << PROTO_COLOR_CPU) | (1u << PROTO_COLOR_GPU_TEMP), msg.stats.colors_mask);
}

void test_stats_colors_that_are_not_an_object_or_not_whole_rgb_are_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":[16757504]"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":\"#ffb300\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":null"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":{\"cpu\":16777216}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":{\"cpu\":-1}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":{\"cpu\":\"#ffb300\"}"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, stats_with("\"time\":1,\"colors\":{\"cpu\":null}"));
}

void test_stats_color_for_a_gpu_with_no_gpu_field_is_accepted_and_kept(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, stats_with("\"time\":1,\"colors\":{\"gpu\":2001125}"));
    TEST_ASSERT_FALSE(msg.stats.has_gpu);
    TEST_ASSERT_EQUAL_UINT8(1u << PROTO_COLOR_GPU, msg.stats.colors_mask);
}

// The caps link.c sends today. When it gains "sleep", the five-cap test below is the one to match.
void test_hello_with_the_four_real_caps_and_a_32_character_fw_is_226_bytes(void)
{
    char buf[256];
    const char *const caps[] = {"fonts", "pages", "status_bar", "feed"};
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, run_of('v', 32), 1024, 600, caps, 4);
    TEST_ASSERT_EQUAL_size_t(226, len);
}

void test_hello_with_five_caps_and_a_32_character_fw_is_234_bytes(void)
{
    char buf[256];
    const char *const caps[] = {"fonts", "pages", "status_bar", "sleep", "feed"};
    size_t len = proto_build_hello(buf, sizeof(buf), "wd-e8f60ae276ab", TOKEN, run_of('v', 32), 1024, 600, caps, 5);
    TEST_ASSERT_EQUAL_size_t(234, len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"caps\":[\"fonts\",\"pages\",\"status_bar\",\"sleep\",\"feed\"]"));
}

// ---- Sleep, feed and asset frames ----

// Parse `base` with the first `from` replaced by `to`. The test fails if `from` is not in `base`.
static esp_err_t parse_edited(const char *base, const char *from, const char *to)
{
    static char edited[PROTO_MAX_MSG];
    const char *at = strstr(base, from);
    TEST_ASSERT_NOT_NULL(at);
    int n = snprintf(edited, sizeof(edited), "%.*s%s%s", (int)(at - base), base, to, at + strlen(from));
    TEST_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(edited));
    return proto_parse(edited, (size_t)n, &msg);
}

// The fixtures in app/src-tauri/tests/fixtures/, which the daemon round-trips.
static const char SLEEP_FIXTURE[] =
    "{\"t\":\"sleep\",\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\",\"bg\":0,\"fg\":16777215,"
    "\"show\":[\"clock\",\"date\",\"host\"],\"text\":\"Back soon\",\"anchor\":\"bottom_left\",\"spinner\":\"arc\","
    "\"spinner_color\":3718648,\"offline_after_s\":10,\"backlight\":100}";
static const char SLEEP_MIN_FIXTURE[] =
    "{\"t\":\"sleep\",\"bg\":0,\"fg\":0,\"show\":[],\"anchor\":\"center\",\"spinner\":\"none\",\"spinner_color\":0,"
    "\"offline_after_s\":0,\"backlight\":0}";
static const char FEED_FIXTURE[] =
    "{\"t\":\"feed\",\"sources\":2,\"cards\":[{\"id\":\"nas-load\",\"label\":\"NAS load\",\"value\":\"0.42\","
    "\"state\":\"ok\"}],\"notes\":[{\"id\":\"ha/backup\",\"title\":\"Backup done\","
    "\"body\":\"Nightly backup finished in 4 min\",\"level\":\"ok\",\"src\":\"Home Assistant\","
    "\"at\":1792692433}]}";

void test_sleep_fixture_has_every_field(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(SLEEP_FIXTURE, strlen(SLEEP_FIXTURE), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_SLEEP, msg.type);
    const proto_sleep_t *s = &msg.sleep;
    TEST_ASSERT_TRUE(s->has_image);
    const uint8_t image[PROTO_ASSET_ID_LEN] = {0x5b, 0x1f, 0x0c, 0x7e, 0x9d, 0x3a, 0x2b, 0x4c,
                                               0x6d, 0x8e, 0x0f, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(image, s->image, PROTO_ASSET_ID_LEN);
    TEST_ASSERT_EQUAL_UINT32(0, s->bg);
    TEST_ASSERT_EQUAL_UINT32(16777215u, s->fg);
    TEST_ASSERT_EQUAL_UINT8(PROTO_SHOW_CLOCK | PROTO_SHOW_DATE | PROTO_SHOW_HOST, s->show_mask);
    TEST_ASSERT_EQUAL_STRING("Back soon", s->text);
    TEST_ASSERT_EQUAL(PROTO_ANCHOR_BOTTOM_LEFT, s->anchor);
    TEST_ASSERT_EQUAL(PROTO_SPINNER_ARC, s->spinner);
    TEST_ASSERT_EQUAL_UINT32(3718648u, s->spinner_color);
    TEST_ASSERT_EQUAL_UINT32(10, s->offline_after_s);
    TEST_ASSERT_EQUAL_UINT32(100, s->backlight);
}

void test_sleep_minimal_fixture_has_no_image_or_text(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(SLEEP_MIN_FIXTURE, strlen(SLEEP_MIN_FIXTURE), &msg));
    TEST_ASSERT_FALSE(msg.sleep.has_image);
    TEST_ASSERT_EQUAL_STRING("", msg.sleep.text);
    TEST_ASSERT_EQUAL_UINT8(0, msg.sleep.show_mask);
    TEST_ASSERT_EQUAL(PROTO_ANCHOR_CENTER, msg.sleep.anchor);
    TEST_ASSERT_EQUAL(PROTO_SPINNER_NONE, msg.sleep.spinner);
}

void test_sleep_image_must_be_32_lowercase_hex(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\"",
                                                        "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\"",
                                                        "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e0\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\"",
                                                        "\"image\":\"5B1F0C7E9D3A2B4C6D8E0F1A2B3C4D5E\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"image\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\"", "\"image\":null"));
}

void test_sleep_show_names_once_and_unknown_names_are_ignored(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "[\"clock\",\"date\",\"host\"]", "[\"clock\",\"clock\"]"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "[\"clock\",\"date\",\"host\"]", "[1]"));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "[\"clock\",\"date\",\"host\"]", "[\"clock\",\"weather\"]"));
    TEST_ASSERT_EQUAL_UINT8(PROTO_SHOW_CLOCK, msg.sleep.show_mask);
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "[\"clock\",\"date\",\"host\"]", "[\"text\",\"host\"]"));
    TEST_ASSERT_EQUAL_UINT8(PROTO_SHOW_TEXT | PROTO_SHOW_HOST, msg.sleep.show_mask);
}

void test_sleep_text_of_47_is_accepted_and_48_refused(void)
{
    char field[64];
    snprintf(field, sizeof(field), "\"text\":\"%s\"", run_of('a', 47));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", field));
    TEST_ASSERT_EQUAL_size_t(47, strlen(msg.sleep.text));
    snprintf(field, sizeof(field), "\"text\":\"%s\"", run_of('a', 48));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", field));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", "\"text\":\"a\\nb\""));
}

void test_sleep_colors_and_ranges_are_inclusive(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"fg\":16777215", "\"fg\":16777216"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"spinner_color\":3718648", "\"spinner_color\":-1"));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "\"offline_after_s\":10", "\"offline_after_s\":3600"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"offline_after_s\":10", "\"offline_after_s\":3601"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"offline_after_s\":10", "\"offline_after_s\":-1"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"offline_after_s\":10", "\"offline_after_s\":10.5"));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "\"backlight\":100", "\"backlight\":0"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"backlight\":100", "\"backlight\":101"));
}

void test_sleep_unknown_spinner_or_anchor_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"spinner\":\"arc\"", "\"spinner\":\"wave\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"anchor\":\"bottom_left\"", "\"anchor\":\"middle\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(SLEEP_FIXTURE, "\"anchor\":\"bottom_left\",", ""));
}

// A feed frame with `cards` copies of one valid card and `notes` copies of one valid note whose title is `title`.
static esp_err_t parse_feed_of(size_t cards, size_t notes, const char *title)
{
    static const char CARD[] = "{\"id\":\"nas-load\",\"label\":\"NAS load\",\"value\":\"0.42\",\"state\":\"ok\"}";
    size_t n = (size_t)snprintf(json, sizeof(json), "{\"t\":\"feed\",\"sources\":2,\"cards\":[");
    for (size_t i = 0; i < cards; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n, "%s%s", i ? "," : "", CARD);
    }
    n += (size_t)snprintf(json + n, sizeof(json) - n, "],\"notes\":[");
    for (size_t i = 0; i < notes; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
                              "%s{\"id\":\"ha/backup\",\"title\":\"%s\",\"body\":\"Nightly\",\"level\":\"ok\","
                              "\"src\":\"Home Assistant\",\"at\":1792692433}",
                              i ? "," : "", title);
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    return proto_parse(json, strlen(json), &msg);
}

void test_feed_fixture_has_its_card_and_note(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(FEED_FIXTURE, strlen(FEED_FIXTURE), &msg));
    TEST_ASSERT_EQUAL(PROTO_MSG_FEED, msg.type);
    const proto_feed_t *f = &msg.feed;
    TEST_ASSERT_EQUAL_UINT8(2, f->sources);
    TEST_ASSERT_EQUAL_size_t(1, f->n_cards);
    TEST_ASSERT_EQUAL_STRING("nas-load", f->cards[0].id);
    TEST_ASSERT_EQUAL_STRING("NAS load", f->cards[0].label);
    TEST_ASSERT_EQUAL_STRING("0.42", f->cards[0].value);
    TEST_ASSERT_EQUAL(PROTO_TILE_OK, f->cards[0].state);
    TEST_ASSERT_EQUAL_size_t(1, f->n_notes);
    TEST_ASSERT_EQUAL_STRING("ha/backup", f->notes[0].id);
    TEST_ASSERT_EQUAL_STRING("Backup done", f->notes[0].title);
    TEST_ASSERT_EQUAL_STRING("Nightly backup finished in 4 min", f->notes[0].body);
    TEST_ASSERT_EQUAL(PROTO_LEVEL_OK, f->notes[0].level);
    TEST_ASSERT_EQUAL_STRING("Home Assistant", f->notes[0].src);
    TEST_ASSERT_EQUAL_UINT64(1792692433u, f->notes[0].at);
}

void test_feed_empty_frame_has_no_cards_or_notes(void)
{
    const char *empty = "{\"t\":\"feed\",\"sources\":0,\"cards\":[],\"notes\":[]}";
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse(empty, strlen(empty), &msg));
    TEST_ASSERT_EQUAL_UINT8(0, msg.feed.sources);
    TEST_ASSERT_EQUAL_size_t(0, msg.feed.n_cards);
    TEST_ASSERT_EQUAL_size_t(0, msg.feed.n_notes);
}

void test_feed_9_cards_or_13_notes_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_feed_of(9, 0, "t"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_feed_of(0, 13, "t"));
    TEST_ASSERT_EQUAL(ESP_OK, parse_feed_of(8, 12, "t"));
    TEST_ASSERT_EQUAL_size_t(8, msg.feed.n_cards);
    TEST_ASSERT_EQUAL_size_t(12, msg.feed.n_notes);
}

void test_feed_title_of_47_is_accepted_and_48_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_feed_of(0, 1, run_of('t', 47)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_feed_of(0, 1, run_of('t', 48)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_feed_of(0, 1, ""));
}

void test_feed_string_limits_are_exact(void)
{
    char field[256];
    snprintf(field, sizeof(field), "\"label\":\"%s\"", run_of('l', 23));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"label\":\"NAS load\"", field));
    snprintf(field, sizeof(field), "\"label\":\"%s\"", run_of('l', 24));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"label\":\"NAS load\"", field));
    snprintf(field, sizeof(field), "\"value\":\"%s\"", run_of('v', 15));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"value\":\"0.42\"", field));
    snprintf(field, sizeof(field), "\"value\":\"%s\"", run_of('v', 16));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"value\":\"0.42\"", field));
    snprintf(field, sizeof(field), "\"id\":\"%s\"", run_of('i', 63));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"id\":\"nas-load\"", field));
    snprintf(field, sizeof(field), "\"id\":\"%s\"", run_of('i', 64));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"id\":\"nas-load\"", field));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"id\":\"nas-load\"", "\"id\":\"\""));
    snprintf(field, sizeof(field), "\"body\":\"%s\"", run_of('b', 160));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(FEED_FIXTURE, "\"body\":\"Nightly backup finished in 4 min\"", field));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(FEED_FIXTURE, "\"src\":\"Home Assistant\"", "\"src\":\"Home\\u0001\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"sources\":2", "\"sources\":17"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"sources\":2", "\"sources\":2.5"));
}

void test_feed_unknown_level_is_refused_and_unknown_state_is_unknown(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"level\":\"ok\"", "\"level\":\"debug\""));
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"state\":\"ok\"", "\"state\":\"warn\""));
    TEST_ASSERT_EQUAL(PROTO_TILE_UNKNOWN, msg.feed.cards[0].state);
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"state\":\"ok\"", "\"state\":\"bad\""));
    TEST_ASSERT_EQUAL(PROTO_TILE_BAD, msg.feed.cards[0].state);
}

void test_feed_state_unknown_is_unknown(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(FEED_FIXTURE, "\"state\":\"ok\"", "\"state\":\"unknown\""));
    TEST_ASSERT_EQUAL(PROTO_TILE_UNKNOWN, msg.feed.cards[0].state);
}

void test_nul_escape_is_refused_in_any_string_but_an_escaped_backslash_is_text(void)
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", "\"text\":\"Back\\u0000soon\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"body\":\"Nightly backup finished in 4 min\"",
                                                        "\"body\":\"Nightly\\u0000\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(FEED_FIXTURE, "\"id\":\"nas-load\"", "\"id\":\"nas\\u0000load\""));
    // \\u0000 is a backslash and the text u0000, which is printable, so it is accepted.
    TEST_ASSERT_EQUAL(ESP_OK, parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", "\"text\":\"a\\\\u0000b\""));
    TEST_ASSERT_EQUAL_STRING("a\\u0000b", msg.sleep.text);
}

void test_raw_and_escaped_non_ascii_is_refused(void)
{
    // A raw UTF-8 e-acute (C3 A9) and its é escape both decode to bytes above 0x7e.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", "\"text\":\"ol\xc3\xa9\""));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      parse_edited(SLEEP_FIXTURE, "\"text\":\"Back soon\"", "\"text\":\"ol\\u00e9\""));
}

// Every string at its limit: 8 cards and 12 notes, 63-character ids, 159-character bodies.
static esp_err_t parse_fullest_feed(void)
{
    static char x[PROTO_FEED_BODY_MAX];
    memset(x, 'x', sizeof(x) - 1);
    x[sizeof(x) - 1] = '\0';
    size_t n = (size_t)snprintf(json, sizeof(json), "{\"t\":\"feed\",\"sources\":16,\"cards\":[");
    for (int i = 0; i < 8; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
                              "%s{\"id\":\"%.*s\",\"label\":\"%.*s\",\"value\":\"%.*s\",\"state\":\"bad\"}",
                              i ? "," : "", 63, x, 23, x, 15, x);
    }
    n += (size_t)snprintf(json + n, sizeof(json) - n, "],\"notes\":[");
    for (int i = 0; i < 12; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
                              "%s{\"id\":\"%.*s\",\"title\":\"%.*s\",\"body\":\"%.*s\",\"level\":\"error\","
                              "\"src\":\"%.*s\",\"at\":9007199254740991}",
                              i ? "," : "", 63, x, 47, x, 159, x, 23, x);
    }
    snprintf(json + n, sizeof(json) - n, "]}");
    return proto_parse(json, strlen(json), &msg);
}

void test_feed_fullest_snapshot_parses_and_at_past_2_to_53_is_refused(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, parse_fullest_feed());
    TEST_ASSERT_EQUAL_size_t(8, msg.feed.n_cards);
    TEST_ASSERT_EQUAL_size_t(12, msg.feed.n_notes);
    TEST_ASSERT_EQUAL_size_t(15, strlen(msg.feed.cards[7].value));
    TEST_ASSERT_EQUAL_size_t(159, strlen(msg.feed.notes[11].body));
    TEST_ASSERT_EQUAL_UINT64(9007199254740991ull, msg.feed.notes[11].at);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse_edited(FEED_FIXTURE, "\"at\":1792692433", "\"at\":9007199254740992"));
}

// An asset frame for the chunk at `offset` of ASSET_ID, `data_len` bytes long, in `frame`.
static const uint8_t ASSET_ID[PROTO_ASSET_ID_LEN] = {0x5b, 0x1f, 0x0c, 0x7e, 0x9d, 0x3a, 0x2b, 0x4c,
                                                     0x6d, 0x8e, 0x0f, 0x1a, 0x2b, 0x3c, 0x4d, 0x5e};

static size_t make_asset(uint32_t offset, size_t data_len)
{
    memcpy(frame, "WDA1", 4);
    memcpy(frame + 4, ASSET_ID, PROTO_ASSET_ID_LEN);
    frame[20] = offset & 0xff;
    frame[21] = (offset >> 8) & 0xff;
    frame[22] = (offset >> 16) & 0xff;
    frame[23] = (offset >> 24) & 0xff;
    for (size_t i = 0; i < data_len; i++) {
        frame[PROTO_ASSET_HEADER_LEN + i] = (uint8_t)(i * 7);
    }
    return PROTO_ASSET_HEADER_LEN + data_len;
}

void test_asset_wrong_magic_or_unasked_id_or_offset_is_refused(void)
{
    proto_asset_t asset;
    size_t len = make_asset(0, PROTO_ASSET_CHUNK);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse_asset(frame, len, ASSET_ID, 0, 2 * PROTO_ASSET_CHUNK, &asset));

    frame[3] = '2';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse_asset(frame, len, ASSET_ID, 0, 2 * PROTO_ASSET_CHUNK, &asset));
    frame[3] = '1';

    frame[4] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, proto_parse_asset(frame, len, ASSET_ID, 0, 2 * PROTO_ASSET_CHUNK, &asset));
    frame[4] ^= 0x01;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, 2 * PROTO_ASSET_CHUNK, &asset));
}

void test_asset_length_must_be_the_chunk_or_what_is_left(void)
{
    proto_asset_t asset;
    // A short middle chunk and a long one are both refused.
    size_t len = make_asset(PROTO_ASSET_CHUNK, PROTO_ASSET_CHUNK - 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, 3 * PROTO_ASSET_CHUNK, &asset));
    len = make_asset(PROTO_ASSET_CHUNK, PROTO_ASSET_CHUNK + 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, 3 * PROTO_ASSET_CHUNK, &asset));

    // The last chunk is what is left: 10 bytes of an asset that is one chunk and 10 bytes.
    len = make_asset(PROTO_ASSET_CHUNK, 10);
    TEST_ASSERT_EQUAL(ESP_OK,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, PROTO_ASSET_CHUNK + 10, &asset));
    TEST_ASSERT_EQUAL_size_t(10, asset.len);
    TEST_ASSERT_EQUAL_PTR(frame + PROTO_ASSET_HEADER_LEN, asset.data);
    TEST_ASSERT_EQUAL_UINT32(PROTO_ASSET_CHUNK, asset.offset);
    len = make_asset(PROTO_ASSET_CHUNK, 11);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, PROTO_ASSET_CHUNK + 10, &asset));

    // A frame shorter than its header.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, proto_parse_asset(frame, 23, ASSET_ID, 0, PROTO_ASSET_CHUNK, &asset));
}

void test_asset_full_frame_is_accepted_at_the_cap(void)
{
    proto_asset_t asset;
    size_t len = make_asset(0, PROTO_ASSET_CHUNK);
    TEST_ASSERT_EQUAL_size_t(PROTO_ASSET_FRAME_MAX, len);
    TEST_ASSERT_EQUAL(ESP_OK, proto_parse_asset(frame, len, ASSET_ID, 0, 2 * PROTO_ASSET_CHUNK, &asset));
    TEST_ASSERT_EQUAL_size_t(PROTO_ASSET_CHUNK, asset.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ASSET_ID, asset.id, PROTO_ASSET_ID_LEN);
}

void test_asset_offset_past_the_end_of_the_asset_is_refused(void)
{
    proto_asset_t asset;
    size_t len = make_asset(PROTO_ASSET_CHUNK, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      proto_parse_asset(frame, len, ASSET_ID, PROTO_ASSET_CHUNK, PROTO_ASSET_CHUNK, &asset));
}

void test_asset_get_builds_a_whole_chunk_offset_only(void)
{
    char buf[256];
    size_t len = proto_build_asset_get(buf, sizeof(buf), ASSET_ID, PROTO_ASSET_CHUNK);
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"t\":\"asset_get\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"id\":\"5b1f0c7e9d3a2b4c6d8e0f1a2b3c4d5e\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"offset\":262144"));
    TEST_ASSERT_EQUAL_size_t(0, proto_build_asset_get(buf, sizeof(buf), ASSET_ID, PROTO_ASSET_CHUNK + 1));
}

void test_feed_dismiss_id_is_1_to_63_printable(void)
{
    char buf[256];
    size_t len = proto_build_feed_dismiss(buf, sizeof(buf), "ha/backup");
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\"t\":\"feed_dismiss\",\"id\":\"ha/backup\"}"));
    TEST_ASSERT_EQUAL_size_t(0, proto_build_feed_dismiss(buf, sizeof(buf), ""));
    TEST_ASSERT_EQUAL_size_t(0, proto_build_feed_dismiss(buf, sizeof(buf), run_of('d', 64)));
    TEST_ASSERT_GREATER_THAN(0, proto_build_feed_dismiss(buf, sizeof(buf), run_of('d', 63)));
    TEST_ASSERT_EQUAL_size_t(0, proto_build_feed_dismiss(buf, sizeof(buf), "a\tb"));
}

void app_main(void)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        printf("psa_crypto_init failed: %d\n", (int)status);
        exit(1);
    }

    UNITY_BEGIN();
    RUN_TEST(test_icon_shorter_than_its_header_is_refused);
    RUN_TEST(test_icon_bad_magic_is_refused);
    RUN_TEST(test_icon_side_outside_1_to_256_is_refused);
    RUN_TEST(test_icon_length_one_byte_off_is_refused);
    RUN_TEST(test_icon_flipped_pixel_fails_the_id_check);
    RUN_TEST(test_icon_valid_1x1_is_accepted);
    RUN_TEST(test_icon_valid_256x256_is_accepted);
    RUN_TEST(test_tile_btn_missing_is_refused);
    RUN_TEST(test_tile_btn_of_63_is_accepted_and_64_is_refused);
    RUN_TEST(test_tile_text_as_a_number_is_refused);
    RUN_TEST(test_tile_text_longer_than_64_is_cut_to_64);
    RUN_TEST(test_tile_empty_text_is_accepted);
    RUN_TEST(test_tile_state_absent_weird_or_a_number_is_unknown);
    RUN_TEST(test_tile_btn_with_a_nul_escape_is_cut_at_the_nul);
    RUN_TEST(test_report_crash_has_every_field_with_the_right_type);
    RUN_TEST(test_report_reset_sends_only_id_kind_and_reason);
    RUN_TEST(test_report_invalid_fields_are_refused);
    RUN_TEST(test_report_task_of_16_fits_and_17_does_not);
    RUN_TEST(test_report_too_small_a_buffer_returns_zero);
    RUN_TEST(test_layout_ack_carries_the_rev);
    RUN_TEST(test_report_ack_valid_id_is_parsed);
    RUN_TEST(test_report_ack_bad_id_is_refused);
    RUN_TEST(test_general_unknown_t_is_parsed_as_unknown);
    RUN_TEST(test_general_t_that_is_not_a_string_is_refused);
    RUN_TEST(test_general_root_that_is_not_an_object_is_refused);
    RUN_TEST(test_general_truncated_json_is_refused);
    RUN_TEST(test_general_trailing_garbage_is_accepted);
    RUN_TEST(test_general_two_objects_back_to_back_read_the_first);
    RUN_TEST(test_layout_cols_outside_1_to_8_is_refused);
    RUN_TEST(test_layout_rows_of_6_is_refused);
    RUN_TEST(test_layout_40_buttons_is_accepted_and_41_is_refused);
    RUN_TEST(test_layout_col_equal_to_cols_is_refused);
    RUN_TEST(test_layout_icon_null_is_refused);
    RUN_TEST(test_layout_icon_must_be_32_lowercase_hex);
    RUN_TEST(test_layout_tile_as_a_string_is_refused);
    RUN_TEST(test_layout_rev_of_1e400_or_minus_1_is_refused);
    RUN_TEST(test_layout_fractions_are_truncated);
    RUN_TEST(test_layout_nesting_depth_9_is_refused);
    RUN_TEST(test_layout_label_cut_inside_a_utf8_sequence_leaves_a_lone_lead_byte);
    RUN_TEST(test_font_none_given_is_montserrat_without_a_flag);
    RUN_TEST(test_font_on_the_message_covers_every_label_and_value);
    RUN_TEST(test_font_label_font_carries_over_to_the_value_unless_value_font_is_given);
    RUN_TEST(test_font_every_id_is_known);
    RUN_TEST(test_font_unknown_or_mistyped_values_fall_back_to_montserrat_and_set_the_flag);
    RUN_TEST(test_font_flag_does_not_stick_to_the_next_layout);
    RUN_TEST(test_font_fullest_layout_with_every_font_field_parses);
    RUN_TEST(test_page_none_given_is_page_0_of_1_with_an_empty_id);
    RUN_TEST(test_plain_layout_after_a_paged_one_resets_to_page_0_of_1);
    RUN_TEST(test_page_3_of_4_is_accepted);
    RUN_TEST(test_page_0_of_1_is_accepted);
    RUN_TEST(test_page_equal_to_pages_is_refused);
    RUN_TEST(test_pages_of_0_or_5_is_refused);
    RUN_TEST(test_page_without_pages_or_without_page_id_is_refused);
    RUN_TEST(test_pages_or_page_id_without_page_is_refused);
    RUN_TEST(test_page_id_must_be_1_to_63_bytes);
    RUN_TEST(test_page_fields_of_the_wrong_type_are_refused);
    RUN_TEST(test_page_message_resolves_fonts_like_a_single_layout);
    RUN_TEST(test_hello_without_caps_has_no_caps_key);
    RUN_TEST(test_hello_with_fonts_names_it_in_caps);
    RUN_TEST(test_hello_with_two_caps_and_a_32_character_fw_fits_256_bytes);
    RUN_TEST(test_hello_with_three_caps_and_a_32_character_fw_is_219_bytes);
    RUN_TEST(test_stats_full_fixture_has_every_field);
    RUN_TEST(test_stats_time_only_leaves_every_optional_field_out);
    RUN_TEST(test_stats_time_absent_or_null_is_refused);
    RUN_TEST(test_stats_time_as_a_string_negative_or_2_to_the_53_is_refused);
    RUN_TEST(test_stats_time_of_2_to_the_53_minus_1_is_accepted);
    RUN_TEST(test_stats_cpu_0_and_100_are_accepted_and_101_and_minus_1_are_refused);
    RUN_TEST(test_stats_utc_offset_outside_plus_minus_64800_is_refused);
    RUN_TEST(test_stats_mem_used_without_mem_total_is_refused);
    RUN_TEST(test_stats_mem_used_above_mem_total_is_refused_and_equal_is_accepted);
    RUN_TEST(test_stats_net_rx_without_net_tx_is_refused);
    RUN_TEST(test_stats_fraction_in_an_int_field_is_refused);
    RUN_TEST(test_stats_unknown_fields_are_ignored);
    RUN_TEST(test_stats_colors_fixture_has_every_unit_and_color);
    RUN_TEST(test_stats_gpu_0_and_100_accepted_and_101_minus_1_or_a_fraction_refused);
    RUN_TEST(test_stats_cpu_temp_minus_40_to_150_accepted_and_outside_refused);
    RUN_TEST(test_stats_gpu_temp_minus_40_to_150_accepted_and_outside_refused);
    RUN_TEST(test_stats_gpu_and_its_temperatures_are_independent);
    RUN_TEST(test_stats_temp_unit_accepts_only_c_and_f);
    RUN_TEST(test_stats_net_unit_accepts_si_binary_and_bits_only);
    RUN_TEST(test_stats_colors_as_an_object_of_whole_rgb_values);
    RUN_TEST(test_stats_colors_that_are_not_an_object_or_not_whole_rgb_are_refused);
    RUN_TEST(test_stats_color_for_a_gpu_with_no_gpu_field_is_accepted_and_kept);
    RUN_TEST(test_hello_with_the_four_real_caps_and_a_32_character_fw_is_226_bytes);
    RUN_TEST(test_hello_with_five_caps_and_a_32_character_fw_is_234_bytes);
    RUN_TEST(test_sleep_fixture_has_every_field);
    RUN_TEST(test_sleep_minimal_fixture_has_no_image_or_text);
    RUN_TEST(test_sleep_image_must_be_32_lowercase_hex);
    RUN_TEST(test_sleep_show_names_once_and_unknown_names_are_ignored);
    RUN_TEST(test_sleep_text_of_47_is_accepted_and_48_refused);
    RUN_TEST(test_sleep_colors_and_ranges_are_inclusive);
    RUN_TEST(test_sleep_unknown_spinner_or_anchor_is_refused);
    RUN_TEST(test_feed_fixture_has_its_card_and_note);
    RUN_TEST(test_feed_empty_frame_has_no_cards_or_notes);
    RUN_TEST(test_feed_9_cards_or_13_notes_is_refused);
    RUN_TEST(test_feed_title_of_47_is_accepted_and_48_refused);
    RUN_TEST(test_feed_string_limits_are_exact);
    RUN_TEST(test_feed_unknown_level_is_refused_and_unknown_state_is_unknown);
    RUN_TEST(test_feed_state_unknown_is_unknown);
    RUN_TEST(test_nul_escape_is_refused_in_any_string_but_an_escaped_backslash_is_text);
    RUN_TEST(test_raw_and_escaped_non_ascii_is_refused);
    RUN_TEST(test_feed_fullest_snapshot_parses_and_at_past_2_to_53_is_refused);
    RUN_TEST(test_asset_wrong_magic_or_unasked_id_or_offset_is_refused);
    RUN_TEST(test_asset_length_must_be_the_chunk_or_what_is_left);
    RUN_TEST(test_asset_full_frame_is_accepted_at_the_cap);
    RUN_TEST(test_asset_offset_past_the_end_of_the_asset_is_refused);
    RUN_TEST(test_asset_get_builds_a_whole_chunk_offset_only);
    RUN_TEST(test_feed_dismiss_id_is_1_to_63_printable);
    test_grid_run();
    test_pageset_run();
    test_hoststat_run();
    test_logring_run();
    test_diag_run();
    test_uimode_run();
    test_sleepcfg_run();
    test_feedfmt_run();
    test_cardfs_run();
    test_wifitxt_run();
    test_svc_run();
    test_kbmap_run();
    test_wifistate_run();
    test_storagefmt_run();

    // Returning from app_main does not end a linux-target process, so the failure count becomes the exit status.
    exit(UNITY_END());
}
