// Host tests for logring.c: the log ring, its line cleaning and the secret redaction (G3).
#include <stdio.h>
#include <string.h>

#include "logring.h"
#include "unity.h"

#define MARK LOGRING_REDACTED
#define MARK_N (sizeof(MARK) - 1)

static logring_t ring;
static uint8_t store[4096];
static logring_line_t page[128];

static char token[LOGRING_SECRET_MAX + 1];
static const char *password = "Zp9#qLm2xK";

static void fresh(size_t cap)
{
    TEST_ASSERT_TRUE(logring_init(&ring, store, cap));
}

// Append a copy of `text`, since logring_append works in place.
static void add(const char *text, uint32_t ms)
{
    char scratch[512];
    TEST_ASSERT_TRUE(strlen(text) < sizeof(scratch));
    strcpy(scratch, text);
    logring_append(&ring, scratch, strlen(scratch), ms);
}

static logring_line_t newest(void)
{
    TEST_ASSERT_EQUAL(1, logring_read_before(&ring, logring_next_seq(&ring), false, page, 1));
    return page[0];
}

void test_logring_init_refuses_bad_arguments(void)
{
    TEST_ASSERT_FALSE(logring_init(NULL, store, sizeof(store)));
    TEST_ASSERT_FALSE(logring_init(&ring, NULL, sizeof(store)));
    TEST_ASSERT_FALSE(logring_init(&ring, store, LOGRING_MIN_CAP - 1));
    TEST_ASSERT_TRUE(logring_init(&ring, store, LOGRING_MIN_CAP));
}

void test_logring_levels_from_the_first_character(void)
{
    fresh(sizeof(store));
    add("E (123) link: x", 123);
    add("W (124) link: y", 124);
    add("I (125) link: z", 125);
    add("no prefix at all", 126);
    add("Error: not a level", 127);
    add("E(1) no space", 128);
    TEST_ASSERT_EQUAL(6, logring_read_from(&ring, 0, false, page, 128));
    TEST_ASSERT_EQUAL_CHAR('E', page[0].level);
    TEST_ASSERT_EQUAL_CHAR('W', page[1].level);
    TEST_ASSERT_EQUAL_CHAR('I', page[2].level);
    TEST_ASSERT_EQUAL_CHAR('I', page[3].level);
    TEST_ASSERT_EQUAL_CHAR('I', page[4].level);
    TEST_ASSERT_EQUAL_CHAR('I', page[5].level);
    TEST_ASSERT_EQUAL_STRING("E (123) link: x", page[0].text);
    TEST_ASSERT_EQUAL_UINT8(strlen("E (123) link: x"), page[0].len);
    TEST_ASSERT_EQUAL_UINT32(123, page[0].ms);
    TEST_ASSERT_EQUAL_UINT32(1, page[0].seq);
}

void test_logring_a_300_byte_line_is_cut_at_159(void)
{
    fresh(sizeof(store));
    char line[301];
    memset(line, 'a', 300);
    line[300] = '\0';
    add(line, 0);
    const logring_line_t l = newest();
    TEST_ASSERT_EQUAL_UINT8(LOGRING_TEXT_MAX, l.len);
    TEST_ASSERT_EQUAL(LOGRING_TEXT_MAX, strlen(l.text));
}

void test_logring_strips_ansi_the_line_end_and_non_ascii(void)
{
    fresh(sizeof(store));
    add("\033[0;31mE (5) t: red\033[0m\n", 0);
    add("W (6) t: cl\xc3\xa9 \x01 tab\there\r\n", 0);
    add("I (7) t: lone \033 escape", 0);
    TEST_ASSERT_EQUAL(3, logring_read_from(&ring, 0, false, page, 128));
    TEST_ASSERT_EQUAL_STRING("E (5) t: red", page[0].text);
    TEST_ASSERT_EQUAL_CHAR('E', page[0].level);
    TEST_ASSERT_EQUAL_STRING("W (6) t: cl?? ? tab?here", page[1].text);
    TEST_ASSERT_EQUAL_STRING("I (7) t: lone  escape", page[2].text);
}

// Line n is "line n:" padded with x to a length that depends on n, so a torn or misplaced record shows.
static size_t len_of(uint32_t n)
{
    return 12 + (n * 37) % 100;
}

static void add_numbered(uint32_t n)
{
    char text[200];
    const int h = snprintf(text, sizeof(text), "line %u:", (unsigned)n);
    memset(text + h, 'x', len_of(n) - h);
    text[len_of(n)] = '\0';
    add(text, n);
}

static void check_numbered(const logring_line_t *l)
{
    char want[200];
    const uint32_t n = l->seq; // line n is the n-th appended, which gets seq n
    const int h = snprintf(want, sizeof(want), "line %u:", (unsigned)n);
    memset(want + h, 'x', len_of(n) - h);
    want[len_of(n)] = '\0';
    TEST_ASSERT_EQUAL_STRING(want, l->text);
    TEST_ASSERT_EQUAL_UINT32(n, l->ms);
}

void test_logring_wrap_keeps_whole_lines_and_sequence_grows(void)
{
    fresh(700);
    for (uint32_t n = 1; n <= 500; n++) {
        add_numbered(n);
        TEST_ASSERT_EQUAL_UINT32(n + 1, logring_next_seq(&ring));
        const size_t got = logring_read_from(&ring, 0, false, page, 128);
        TEST_ASSERT_EQUAL(logring_count(&ring), got);
        TEST_ASSERT_EQUAL_UINT32(n, page[got - 1].seq);
        for (size_t i = 0; i < got; i++) {
            TEST_ASSERT_EQUAL_UINT32(page[got - 1].seq - (got - 1 - i), page[i].seq); // consecutive
            check_numbered(&page[i]);
        }
    }
    TEST_ASSERT_TRUE(logring_count(&ring) < 500); // it wrapped, and dropped the oldest
    TEST_ASSERT_TRUE(logring_count(&ring) > 4);
}

void test_logring_a_page_read_again_after_more_lines_is_the_same(void)
{
    fresh(sizeof(store));
    for (uint32_t n = 1; n <= 40; n++) {
        add_numbered(n);
    }
    static logring_line_t first[8], again[8];
    TEST_ASSERT_EQUAL(8, logring_read_from(&ring, 30, false, first, 8));
    TEST_ASSERT_EQUAL_UINT32(30, first[0].seq);
    for (uint32_t n = 41; n <= 60; n++) {
        add_numbered(n);
    }
    TEST_ASSERT_EQUAL(8, logring_read_from(&ring, 30, false, again, 8));
    TEST_ASSERT_EQUAL_MEMORY(first, again, sizeof(first));
}

void test_logring_read_before_pages_backwards_without_gaps(void)
{
    fresh(sizeof(store));
    for (uint32_t n = 1; n <= 20; n++) {
        add_numbered(n);
    }
    TEST_ASSERT_EQUAL(5, logring_read_before(&ring, logring_next_seq(&ring), false, page, 5));
    TEST_ASSERT_EQUAL_UINT32(16, page[0].seq);
    TEST_ASSERT_EQUAL_UINT32(20, page[4].seq);
    TEST_ASSERT_EQUAL(5, logring_read_before(&ring, 16, false, page, 5));
    TEST_ASSERT_EQUAL_UINT32(11, page[0].seq);
    TEST_ASSERT_EQUAL_UINT32(15, page[4].seq);
    TEST_ASSERT_EQUAL(2, logring_read_before(&ring, 3, false, page, 5)); // fewer than a page left
    TEST_ASSERT_EQUAL_UINT32(1, page[0].seq);
    TEST_ASSERT_EQUAL(0, logring_read_before(&ring, 1, false, page, 5));
}

void test_logring_errors_only_keeps_e_and_w(void)
{
    fresh(sizeof(store));
    add("I (1) a: info", 0);
    add("E (2) a: bad", 0);
    add("I (3) a: info", 0);
    add("W (4) a: warn", 0);
    add("I (5) a: info", 0);
    TEST_ASSERT_EQUAL(2, logring_read_from(&ring, 0, true, page, 128));
    TEST_ASSERT_EQUAL_STRING("E (2) a: bad", page[0].text);
    TEST_ASSERT_EQUAL_STRING("W (4) a: warn", page[1].text);
    TEST_ASSERT_EQUAL(1, logring_read_before(&ring, 4, true, page, 1));
    TEST_ASSERT_EQUAL_STRING("E (2) a: bad", page[0].text);
}

/* ---- Redaction ---- */

static void init_secrets(void)
{
    for (int i = 0; i < 64; i++) {
        token[i] = "0123456789abcdef"[(i * 7 + 3) % 16];
    }
    token[64] = '\0';
}

void test_logring_redact_replaces_whole_secrets(void)
{
    char t[LOGRING_FMT_BUF];
    snprintf(t, sizeof(t), "a %s b %s c %s", token, password, token);
    const size_t n = logring_redact(t, strlen(t), sizeof(t), false, token, password);
    TEST_ASSERT_EQUAL_STRING("a <redacted> b <redacted> c <redacted>", t);
    TEST_ASSERT_EQUAL(strlen(t), n);
}

void test_logring_redact_a_short_password_is_left_alone(void)
{
    char t[LOGRING_FMT_BUF] = "pass abc here";
    TEST_ASSERT_EQUAL(13, logring_redact(t, 13, sizeof(t), true, "abc", NULL));
    TEST_ASSERT_EQUAL_STRING("pass abc here", t);
    fresh(sizeof(store));
    logring_set_password(&ring, "abc");
    TEST_ASSERT_EQUAL_STRING("", ring.pass);
    logring_set_password(&ring, "abcd");
    TEST_ASSERT_EQUAL_STRING("abcd", ring.pass);
    logring_set_password(&ring, NULL);
    TEST_ASSERT_EQUAL_STRING("", ring.pass);
}

void test_logring_redact_growth_stays_inside_the_buffer(void)
{
    char t[16] = "aaaa aaaa aaaa";
    const size_t n = logring_redact(t, 14, sizeof(t), false, "aaaa", NULL);
    TEST_ASSERT_TRUE(n <= sizeof(t) - 1);
    TEST_ASSERT_EQUAL(n, strlen(t));
    TEST_ASSERT_EQUAL_MEMORY(MARK, t, MARK_N);
}

// The stored line must not end in, or hold, 4 or more characters of the secret.
static void assert_no_secret(const char *line, const char *secret)
{
    const size_t len = strlen(line);
    const size_t sl = strlen(secret);
    TEST_ASSERT_NULL(strstr(line, secret));
    for (size_t k = LOGRING_SECRET_MIN; k < sl && k <= len; k++) {
        TEST_ASSERT_TRUE(memcmp(line + len - k, secret, k) != 0);
    }
}

// Format `pad` dots, the secret and a tail the way vsnprintf into a 224-byte buffer would, and capture it.
static void capture_at(size_t pad, const char *secret)
{
    char full[512];
    memset(full, '.', pad);
    snprintf(full + pad, sizeof(full) - pad, "%s and then some more text", secret);
    char buf[LOGRING_FMT_BUF];
    const int ret = (int)strlen(full);
    // vsnprintf's result: the first 223 bytes and a NUL
    const size_t kept = (size_t)ret < sizeof(buf) - 1 ? (size_t)ret : sizeof(buf) - 1;
    memcpy(buf, full, kept);
    buf[kept] = '\0';
    TEST_ASSERT_TRUE(logring_capture(&ring, buf, sizeof(buf), ret, 0));
}

static void redaction_at_every_position(const char *secret)
{
    const size_t sl = strlen(secret);
    for (size_t p = 0; p <= LOGRING_FMT_BUF - 1; p++) {
        fresh(sizeof(store));
        logring_set_token(&ring, token);
        logring_set_password(&ring, password);
        capture_at(p, secret);
        const logring_line_t l = newest();
        assert_no_secret(l.text, secret);
        // 4 or more characters of the secret are in the 223-byte text: they must have become the marker, at p
        // (or its first part, when the 159-byte cut ends it)
        const size_t visible = LOGRING_FMT_BUF - 1 - p < sl ? LOGRING_FMT_BUF - 1 - p : sl;
        if (visible >= LOGRING_SECRET_MIN && p < LOGRING_TEXT_MAX) {
            const size_t room = LOGRING_TEXT_MAX - p;
            TEST_ASSERT_EQUAL_MEMORY(MARK, l.text + p, room < MARK_N ? room : MARK_N);
        }
        if (p >= LOGRING_TEXT_MAX) { // wholly past the cut: the line is the dots
            TEST_ASSERT_EQUAL(LOGRING_TEXT_MAX, strlen(l.text));
        }
    }
}

void test_logring_token_redacted_at_every_start_position(void)
{
    redaction_at_every_position(token);
}

void test_logring_password_redacted_at_every_start_position(void)
{
    redaction_at_every_position(password);
}

void test_logring_a_cut_off_prefix_under_4_characters_is_left(void)
{
    char t[LOGRING_FMT_BUF];
    snprintf(t, sizeof(t), "x %.3s", token);
    TEST_ASSERT_EQUAL(5, logring_redact(t, 5, sizeof(t), true, token, password));
    snprintf(t, sizeof(t), "x %.4s", token);
    TEST_ASSERT_EQUAL(2 + MARK_N, logring_redact(t, 6, sizeof(t), true, token, password));
    TEST_ASSERT_EQUAL_STRING("x " MARK, t);
}

void test_logring_prefix_is_only_redacted_when_the_text_was_truncated(void)
{
    fresh(sizeof(store));
    logring_set_password(&ring, password);
    char buf[LOGRING_FMT_BUF];
    snprintf(buf, sizeof(buf), "E (1) t: tried Zp9#");
    TEST_ASSERT_TRUE(logring_capture(&ring, buf, sizeof(buf), (int)strlen(buf), 0));
    TEST_ASSERT_EQUAL_STRING("E (1) t: tried Zp9#", newest().text); // the line really ends there
}

void test_logring_capture_refuses_a_formatter_error(void)
{
    fresh(sizeof(store));
    char buf[LOGRING_FMT_BUF] = "x";
    TEST_ASSERT_FALSE(logring_capture(&ring, buf, sizeof(buf), -1, 0));
    TEST_ASSERT_EQUAL(0, logring_count(&ring));
}

void test_logring_run(void)
{
    init_secrets();
    RUN_TEST(test_logring_init_refuses_bad_arguments);
    RUN_TEST(test_logring_levels_from_the_first_character);
    RUN_TEST(test_logring_a_300_byte_line_is_cut_at_159);
    RUN_TEST(test_logring_strips_ansi_the_line_end_and_non_ascii);
    RUN_TEST(test_logring_wrap_keeps_whole_lines_and_sequence_grows);
    RUN_TEST(test_logring_a_page_read_again_after_more_lines_is_the_same);
    RUN_TEST(test_logring_read_before_pages_backwards_without_gaps);
    RUN_TEST(test_logring_errors_only_keeps_e_and_w);
    RUN_TEST(test_logring_redact_replaces_whole_secrets);
    RUN_TEST(test_logring_redact_a_short_password_is_left_alone);
    RUN_TEST(test_logring_redact_growth_stays_inside_the_buffer);
    RUN_TEST(test_logring_token_redacted_at_every_start_position);
    RUN_TEST(test_logring_password_redacted_at_every_start_position);
    RUN_TEST(test_logring_a_cut_off_prefix_under_4_characters_is_left);
    RUN_TEST(test_logring_prefix_is_only_redacted_when_the_text_was_truncated);
    RUN_TEST(test_logring_capture_refuses_a_formatter_error);
}
