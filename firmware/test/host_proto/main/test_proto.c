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

    // Returning from app_main does not end a linux-target process, so the failure count becomes the exit status.
    exit(UNITY_END());
}
