// Host tests for kbmap.c: the Wi-Fi editor's keyboard maps.
#include <stdbool.h>
#include <string.h>

#include "kbmap.h"
#include "unity.h"

// A key's label, or NULL at the end of the map. "\n" is a row break, not a key.
static const char *next_key(const char *const *map, size_t *i)
{
    while (map[*i][0] == '\n' && map[*i][1] == '\0') {
        (*i)++;
    }
    if (map[*i][0] == '\0') {
        return NULL;
    }
    return map[(*i)++];
}

void test_kbmap_the_three_modes_hold_every_printable_ascii_character(void)
{
    bool seen[128] = {false};
    for (int mode = 0; mode < KBMAP_MODES; mode++) {
        const char *const *map = kbmap_text((kbmap_mode_t)mode);
        size_t i = 0;
        for (const char *key = next_key(map, &i); key; key = next_key(map, &i)) {
            if (strlen(key) == 1) {
                seen[(unsigned char)key[0]] = true;
            }
        }
    }
    for (int c = 0x20; c <= 0x7E; c++) {
        TEST_ASSERT_TRUE_MESSAGE(seen[c], "a printable character has no key");
    }
}

void test_kbmap_no_label_is_empty_or_longer_than_three_bytes(void)
{
    for (int mode = 0; mode < KBMAP_MODES; mode++) {
        const char *const *map = kbmap_text((kbmap_mode_t)mode);
        size_t i = 0;
        for (const char *key = next_key(map, &i); key; key = next_key(map, &i)) {
            TEST_ASSERT_GREATER_THAN(0, strlen(key));
            TEST_ASSERT_LESS_OR_EQUAL(3, strlen(key));
        }
    }
}

void test_kbmap_every_key_has_a_width_and_the_counts_agree(void)
{
    for (int mode = 0; mode < KBMAP_MODES; mode++) {
        const char *const *map = kbmap_text((kbmap_mode_t)mode);
        const uint8_t *keys = kbmap_keys((kbmap_mode_t)mode);
        size_t n = 0;
        size_t i = 0;
        for (const char *key = next_key(map, &i); key; key = next_key(map, &i)) {
            n++;
        }
        TEST_ASSERT_EQUAL_size_t(n, kbmap_key_count((kbmap_mode_t)mode));
        TEST_ASSERT_LESS_OR_EQUAL(KBMAP_MAX_KEYS, n);
        for (size_t k = 0; k < n; k++) {
            TEST_ASSERT_GREATER_THAN(0, keys[k] & KBKEY_WIDTH_MASK);
        }
    }
}

// Each mode offers a way to the other two (or hides the keyboard), so no mode is a dead end.
void test_kbmap_each_mode_reaches_the_others_and_has_backspace_and_ok(void)
{
    const char *const switches[KBMAP_MODES][2] = {
        {KBMAP_MODE_UPPER, KBMAP_MODE_SPECIAL}, // lower
        {KBMAP_MODE_LOWER, KBMAP_MODE_SPECIAL}, // upper
        {KBMAP_MODE_LOWER, NULL},               // special: back to lower, which holds the way to upper
    };
    for (int mode = 0; mode < KBMAP_MODES; mode++) {
        bool found[2] = {false, switches[mode][1] == NULL};
        bool backspace = false, ok = false;
        const char *const *map = kbmap_text((kbmap_mode_t)mode);
        size_t i = 0;
        for (const char *key = next_key(map, &i); key; key = next_key(map, &i)) {
            found[0] |= strcmp(key, switches[mode][0]) == 0;
            found[1] |= switches[mode][1] && strcmp(key, switches[mode][1]) == 0;
            backspace |= strcmp(key, KBMAP_BACKSPACE) == 0;
            ok |= strcmp(key, KBMAP_OK) == 0;
        }
        TEST_ASSERT_TRUE(found[0]);
        TEST_ASSERT_TRUE(found[1]);
        TEST_ASSERT_TRUE(backspace);
        TEST_ASSERT_TRUE(ok);
    }
}

void test_kbmap_run(void)
{
    RUN_TEST(test_kbmap_the_three_modes_hold_every_printable_ascii_character);
    RUN_TEST(test_kbmap_no_label_is_empty_or_longer_than_three_bytes);
    RUN_TEST(test_kbmap_every_key_has_a_width_and_the_counts_agree);
    RUN_TEST(test_kbmap_each_mode_reaches_the_others_and_has_backspace_and_ok);
}
