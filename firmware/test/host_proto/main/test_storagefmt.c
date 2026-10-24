// The Storage tab's row texts (storagefmt.c).
#include <string.h>

#include "storagefmt.h"
#include "unity.h"

static void assert_size(const char *want, uint64_t bytes)
{
    char out[STORAGEFMT_SIZE_MAX];
    storagefmt_size(out, bytes);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_storagefmt_size_steps_from_bytes_to_gigabytes(void)
{
    assert_size("0 B", 0);
    assert_size("1023 B", 1023);
    assert_size("1.0 KB", 1024);
    assert_size("1.5 KB", 1536);
    assert_size("1023.9 KB", 1024 * 1024 - 1); // rounded down, never "1024.0 KB"
    assert_size("1.0 MB", 1024 * 1024);
    assert_size("59.5 GB", (uint64_t)(59.5 * 1024 * 1024 * 1024));
    assert_size("1.0 TB", 1024ull * 1024 * 1024 * 1024);
    assert_size("2048.0 TB", 2048ull * 1024 * 1024 * 1024 * 1024); // the last unit keeps counting
}

static void assert_card(const char *want, bool present, const char *name, uint64_t bytes, const char *fs, bool ro)
{
    char out[128];
    storagefmt_card(out, sizeof(out), present, name, bytes, fs, ro);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_storagefmt_card_row_shows_what_is_known(void)
{
    const uint64_t gb = 1024ull * 1024 * 1024;
    assert_card("Card: none", false, "", 0, NULL, false);
    assert_card("Card: SD64G, 59.0 GB, FAT32", true, "SD64G", 59 * gb, "FAT32", false);
    assert_card("Card: SD64G, 59.0 GB, FAT16, read-only", true, "SD64G", 59 * gb, "FAT16", true);
    assert_card("Card: 59.0 GB", true, "", 59 * gb, NULL, false); // unreadable: no file system, no name
    assert_card("Card: A?B, 1.0 GB", true,
                "A\x01"
                "B",
                gb, NULL, false);
}

static void assert_owner(const char *want, storagefmt_owner_t owner, bool present, bool connected, const char *error)
{
    char out[128];
    storagefmt_owner(out, sizeof(out), owner, present, connected, error);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_storagefmt_owner_row_names_each_state(void)
{
    assert_owner("Owner: on the device", STORAGEFMT_OWNER_DEVICE, true, false, "");
    assert_owner("Owner: shared over USB", STORAGEFMT_OWNER_SHARED, true, true, "");
    assert_owner("Owner: shared, no computer attached", STORAGEFMT_OWNER_SHARED, true, false, "");
    assert_owner("Owner: no card", STORAGEFMT_OWNER_NONE, false, false, "ESP_ERR_TIMEOUT");
    assert_owner("Owner: unreadable: ESP_ERR_NOT_SUPPORTED", STORAGEFMT_OWNER_NONE, true, false,
                 "ESP_ERR_NOT_SUPPORTED");
    assert_owner("Owner: unreadable", STORAGEFMT_OWNER_NONE, true, false, "");
}

static storagefmt_wifi_t parse(const char *text, char ssid[WIFITXT_SSID_MAX + 1])
{
    return storagefmt_wifi_parse(text, strlen(text), ssid);
}

void test_storagefmt_wifi_parse_returns_the_ssid_and_never_the_password(void)
{
    char ssid[WIFITXT_SSID_MAX + 1];
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_FOUND, parse("Home\nsecret12\n", ssid));
    TEST_ASSERT_EQUAL_STRING("Home", ssid);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_FOUND, parse("Cafe\r\nsecret12\r\n", ssid));
    TEST_ASSERT_EQUAL_STRING("Cafe", ssid);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_FOUND, parse("Open net\n", ssid));
    TEST_ASSERT_EQUAL_STRING("Open net", ssid);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_FOUND, parse("NoNewline", ssid));
    TEST_ASSERT_EQUAL_STRING("NoNewline", ssid);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_BLANK, parse("\nsecret12\n", ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
    // Too long a name, a password or no line at all: the file can't be used, and no SSID is passed on.
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_INVALID, parse("012345678901234567890123456789012\nsecret12\n", ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_INVALID, storagefmt_wifi_parse("", 0, ssid));
    char long_pass[200] = "Home\n";
    memset(long_pass + 5, 'p', 100);
    TEST_ASSERT_EQUAL(STORAGEFMT_WIFI_INVALID, storagefmt_wifi_parse(long_pass, 105, ssid));
    TEST_ASSERT_EQUAL_STRING("", ssid);
}

static void assert_wifi(const char *want, storagefmt_access_t access, storagefmt_wifi_t file, const char *ssid)
{
    char out[128];
    storagefmt_wifi(out, sizeof(out), access, file, ssid);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_storagefmt_wifi_row_follows_the_access_then_the_file(void)
{
    assert_wifi("wifi.txt: found, network Home", STORAGEFMT_READ, STORAGEFMT_WIFI_FOUND, "Home");
    assert_wifi("wifi.txt: found, network Net??", STORAGEFMT_READ, STORAGEFMT_WIFI_FOUND, "Net\xc3\xa9");
    assert_wifi("wifi.txt: found, network #ff0000 red#", STORAGEFMT_READ, STORAGEFMT_WIFI_FOUND, "#ff0000 red#");
    assert_wifi("wifi.txt: found, blank (connects to nothing)", STORAGEFMT_READ, STORAGEFMT_WIFI_BLANK, "");
    assert_wifi("wifi.txt: found, but not valid", STORAGEFMT_READ, STORAGEFMT_WIFI_INVALID, "");
    assert_wifi("wifi.txt: not found", STORAGEFMT_READ, STORAGEFMT_WIFI_ABSENT, "");
    assert_wifi("wifi.txt: unknown (card shared)", STORAGEFMT_SHARED, STORAGEFMT_WIFI_FOUND, "Home");
    assert_wifi("wifi.txt: no card", STORAGEFMT_NO_CARD, STORAGEFMT_WIFI_ABSENT, "");
    assert_wifi("wifi.txt: could not be read", STORAGEFMT_ERROR, STORAGEFMT_WIFI_ABSENT, "");
    assert_wifi("wifi.txt: reading...", STORAGEFMT_NOT_READ, STORAGEFMT_WIFI_ABSENT, "");
}

static void assert_sleep(const char *want, storagefmt_access_t access, bool cfg, size_t images, const char *name,
                         uint64_t size)
{
    char out[128];
    storagefmt_sleep(out, sizeof(out), access, cfg, images, name, size);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_storagefmt_sleep_row_lists_the_config_and_the_first_image(void)
{
    assert_sleep("Sleep files: none", STORAGEFMT_READ, false, 0, "", 0);
    assert_sleep("Sleep files: sleep.cfg, no image", STORAGEFMT_READ, true, 0, "", 0);
    assert_sleep("Sleep files: sleep.cfg, 1a2b3c4d.img 1.0 MB", STORAGEFMT_READ, true, 1, "1a2b3c4d.img", 1048576);
    assert_sleep("Sleep files: no sleep.cfg, 1a2b3c4d.img 1.0 MB", STORAGEFMT_READ, false, 1, "1a2b3c4d.img", 1048576);
    assert_sleep("Sleep files: sleep.cfg, 1a2b3c4d.img 1.0 MB and 2 more", STORAGEFMT_READ, true, 3, "1a2b3c4d.img",
                 1048576);
    assert_sleep("Sleep files: unknown (card shared)", STORAGEFMT_SHARED, true, 1, "x.img", 1);
    assert_sleep("Sleep files: no card", STORAGEFMT_NO_CARD, false, 0, "", 0);
    assert_sleep("Sleep files: could not be read", STORAGEFMT_ERROR, false, 0, "", 0);
    assert_sleep("Sleep files: reading...", STORAGEFMT_NOT_READ, false, 0, "", 0);
}

void test_storagefmt_rows_never_write_past_a_short_buffer(void)
{
    char out[24];
    memset(out, 'x', sizeof(out));
    storagefmt_sleep(out, 16, STORAGEFMT_READ, true, 3, "1a2b3c4d.img", 1048576);
    TEST_ASSERT_EQUAL_STRING("Sleep files: sl", out);
    TEST_ASSERT_EQUAL_CHAR('x', out[16]);
    storagefmt_card(out, 10, true, "SD64G", 1ull << 30, "FAT32", true);
    TEST_ASSERT_EQUAL_STRING("Card: SD6", out);
}

void test_storagefmt_free_row(void)
{
    char out[64];
    storagefmt_free(out, sizeof(out), false, 5, 9);
    TEST_ASSERT_EQUAL_STRING("Free space: not checked", out);
    storagefmt_free(out, sizeof(out), true, 3ull << 30, 59ull << 30);
    TEST_ASSERT_EQUAL_STRING("Free space: 3.0 GB of 59.0 GB", out);
}

void test_storagefmt_run(void)
{
    RUN_TEST(test_storagefmt_size_steps_from_bytes_to_gigabytes);
    RUN_TEST(test_storagefmt_card_row_shows_what_is_known);
    RUN_TEST(test_storagefmt_owner_row_names_each_state);
    RUN_TEST(test_storagefmt_wifi_parse_returns_the_ssid_and_never_the_password);
    RUN_TEST(test_storagefmt_wifi_row_follows_the_access_then_the_file);
    RUN_TEST(test_storagefmt_sleep_row_lists_the_config_and_the_first_image);
    RUN_TEST(test_storagefmt_rows_never_write_past_a_short_buffer);
    RUN_TEST(test_storagefmt_free_row);
}
