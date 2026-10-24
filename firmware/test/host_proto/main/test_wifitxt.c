// Host tests for wifitxt.c: the formatter's output read back by the parser wifi.c uses at boot.
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "wifitxt.h"

static char ssid[WIFITXT_SSID_MAX + 1];
static char pass[WIFITXT_PASS_MAX + 1];

static wifitxt_result_t parse(const char *text, size_t len)
{
    FILE *f = fmemopen((void *)text, len, "r");
    TEST_ASSERT_NOT_NULL(f);
    const wifitxt_result_t res = wifitxt_parse(f, ssid, pass);
    fclose(f);
    return res;
}

static wifitxt_result_t parse_str(const char *text)
{
    return parse(text, strlen(text));
}

static void round_trip(const char *in_ssid, const char *in_pass, const char *expect)
{
    char buf[WIFITXT_FILE_MAX];
    const size_t n = wifitxt_format(in_ssid, in_pass, buf);
    TEST_ASSERT_EQUAL(strlen(expect), n);
    TEST_ASSERT_EQUAL_MEMORY(expect, buf, n);
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse(buf, n));
    TEST_ASSERT_EQUAL_STRING(in_ssid, ssid);
    TEST_ASSERT_EQUAL_STRING(in_pass, pass);
}

static const char HEX64[] = "0123456789abcdefABCDEF0123456789abcdef0123456789abcdef0123456789";
static const char PASS63[] = "a password of sixty-three printable characters, spaces too ~!@#";
static const char SSID32[] = "a 32-byte ssid, the longest one.";

void test_wifitxt_format_then_parse_an_open_network(void)
{
    round_trip("Home", "", "Home\n");
}

void test_wifitxt_format_then_parse_8_and_63_character_passwords(void)
{
    TEST_ASSERT_EQUAL(63, strlen(PASS63));
    round_trip("Home", "12345678", "Home\n12345678\n");
    char expect[128];
    sprintf(expect, "Home\n%s\n", PASS63);
    round_trip("Home", PASS63, expect);
}

void test_wifitxt_format_then_parse_a_64_hex_key_and_a_32_byte_ssid(void)
{
    TEST_ASSERT_EQUAL(64, strlen(HEX64));
    TEST_ASSERT_EQUAL(32, strlen(SSID32));
    char expect[128];
    sprintf(expect, "%s\n%s\n", SSID32, HEX64);
    round_trip(SSID32, HEX64, expect);
}

void test_wifitxt_format_refuses_bad_ssids(void)
{
    char buf[WIFITXT_FILE_MAX];
    TEST_ASSERT_EQUAL(0, wifitxt_format("", "12345678", buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("a 33-byte ssid, one byte too long", "12345678", buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("Ho\nme", "12345678", buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home\r", "12345678", buf));
}

void test_wifitxt_format_refuses_bad_passwords(void)
{
    char buf[WIFITXT_FILE_MAX];
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home", "1234567", buf)); // 7 characters
    char nonhex[65];
    memcpy(nonhex, HEX64, 64);
    nonhex[63] = 'g';
    nonhex[64] = '\0';
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home", nonhex, buf)); // 64 characters, not all hex
    char long65[66];
    memset(long65, 'a', 65);
    long65[65] = '\0';
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home", long65, buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home", "1234\n5678", buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home",
                                        "1234\x01"
                                        "5678",
                                        buf));
    TEST_ASSERT_EQUAL(0, wifitxt_format("Home",
                                        "\xc3\xa9t\xc3\xa9"
                                        "1234",
                                        buf)); // not ASCII
}

void test_wifitxt_crlf_parses_as_lf(void)
{
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str("Home\r\nsecret12\r\n"));
    TEST_ASSERT_EQUAL_STRING("Home", ssid);
    TEST_ASSERT_EQUAL_STRING("secret12", pass);
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str("Home\r\n"));
    TEST_ASSERT_EQUAL_STRING("Home", ssid);
    TEST_ASSERT_EQUAL_STRING("", pass);
}

void test_wifitxt_blank_first_line_means_dont_connect(void)
{
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str("\nsecret12\n"));
    TEST_ASSERT_EQUAL_STRING("", ssid);
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str("\r\n"));
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

void test_wifitxt_parse_refuses_long_lines_and_an_empty_file(void)
{
    TEST_ASSERT_EQUAL(WIFITXT_EMPTY, parse("", 0));
    TEST_ASSERT_EQUAL(WIFITXT_SSID_LONG, parse_str("a 33-byte ssid, one byte too long\nsecret12\n"));
    TEST_ASSERT_EQUAL(WIFITXT_SSID_LONG, parse_str("a 33-byte ssid, one byte too long"));
    char text[128];
    sprintf(text, "Home\n%sx\n", HEX64);
    TEST_ASSERT_EQUAL(WIFITXT_PASS_LONG, parse_str(text));
    sprintf(text, "%s\r\n%s\r\n", SSID32, HEX64); // the longest lines, with CRLF
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str(text));
    TEST_ASSERT_EQUAL_STRING(SSID32, ssid);
    TEST_ASSERT_EQUAL_STRING(HEX64, pass);
    TEST_ASSERT_EQUAL(WIFITXT_OK, parse_str(SSID32)); // no final newline
    TEST_ASSERT_EQUAL_STRING(SSID32, ssid);
}

void test_wifitxt_run(void)
{
    RUN_TEST(test_wifitxt_format_then_parse_an_open_network);
    RUN_TEST(test_wifitxt_format_then_parse_8_and_63_character_passwords);
    RUN_TEST(test_wifitxt_format_then_parse_a_64_hex_key_and_a_32_byte_ssid);
    RUN_TEST(test_wifitxt_format_refuses_bad_ssids);
    RUN_TEST(test_wifitxt_format_refuses_bad_passwords);
    RUN_TEST(test_wifitxt_crlf_parses_as_lf);
    RUN_TEST(test_wifitxt_blank_first_line_means_dont_connect);
    RUN_TEST(test_wifitxt_parse_refuses_long_lines_and_an_empty_file);
}
