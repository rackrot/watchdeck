// Host tests for diag.c: the error counters and the Wi-Fi reason names (G3).
#include <string.h>

#include "diag.h"
#include "unity.h"

void test_diag_counts_and_keeps_the_last_reason(void)
{
    diag_entry_t before, after;
    TEST_ASSERT_TRUE(diag_get(DIAG_LINK_DROP, &before));
    diag_count(DIAG_LINK_DROP, "first");
    diag_count(DIAG_LINK_DROP, "second");
    TEST_ASSERT_TRUE(diag_get(DIAG_LINK_DROP, &after));
    TEST_ASSERT_EQUAL_UINT32(before.count + 2, after.count);
    TEST_ASSERT_EQUAL_STRING("second", after.reason);
    diag_count(DIAG_LINK_DROP, NULL); // keeps the reason
    TEST_ASSERT_TRUE(diag_get(DIAG_LINK_DROP, &after));
    TEST_ASSERT_EQUAL_UINT32(before.count + 3, after.count);
    TEST_ASSERT_EQUAL_STRING("second", after.reason);
}

void test_diag_kinds_count_apart(void)
{
    diag_entry_t a, b;
    diag_get(DIAG_ICON_TIMEOUT, &a);
    diag_count(DIAG_CARD_ERROR, "x");
    diag_get(DIAG_ICON_TIMEOUT, &b);
    TEST_ASSERT_EQUAL_UINT32(a.count, b.count);
}

void test_diag_a_long_reason_is_cut_and_terminated(void)
{
    char big[200];
    memset(big, 'r', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    diag_count(DIAG_SET_BROKEN, big);
    diag_entry_t e;
    TEST_ASSERT_TRUE(diag_get(DIAG_SET_BROKEN, &e));
    TEST_ASSERT_EQUAL(DIAG_REASON_MAX - 1, strlen(e.reason));
}

void test_diag_unknown_kind_is_refused(void)
{
    diag_entry_t e;
    TEST_ASSERT_FALSE(diag_get(DIAG_KINDS, &e));
    TEST_ASSERT_FALSE(diag_get((diag_kind_t)-1, &e));
    diag_count(DIAG_KINDS, "nothing"); // must not write out of range
}

void test_diag_every_kind_has_a_name(void)
{
    for (int k = 0; k < DIAG_KINDS; k++) {
        TEST_ASSERT_TRUE(strlen(diag_kind_name((diag_kind_t)k)) > 0);
    }
}

void test_diag_wifi_reason_names(void)
{
    TEST_ASSERT_EQUAL_STRING("network not found", wifi_reason_name(201));
    TEST_ASSERT_EQUAL_STRING("wrong password?", wifi_reason_name(15));
    TEST_ASSERT_EQUAL_STRING("authentication failed", wifi_reason_name(202));
    TEST_ASSERT_EQUAL_STRING("authentication expired", wifi_reason_name(2));
    TEST_ASSERT_EQUAL_STRING("wrong password?", wifi_reason_name(204));
    TEST_ASSERT_EQUAL_STRING("connection failed", wifi_reason_name(205));
    TEST_ASSERT_EQUAL_STRING("other", wifi_reason_name(0));
    TEST_ASSERT_EQUAL_STRING("other", wifi_reason_name(9999));
    TEST_ASSERT_EQUAL_STRING("other", wifi_reason_name(-1));
}

void test_diag_run(void)
{
    RUN_TEST(test_diag_counts_and_keeps_the_last_reason);
    RUN_TEST(test_diag_kinds_count_apart);
    RUN_TEST(test_diag_a_long_reason_is_cut_and_terminated);
    RUN_TEST(test_diag_unknown_kind_is_refused);
    RUN_TEST(test_diag_every_kind_has_a_name);
    RUN_TEST(test_diag_wifi_reason_names);
}
