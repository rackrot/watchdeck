// The feed page's age text and row counts (feedfmt.c).
#include <string.h>

#include "feedfmt.h"
#include "unity.h"

#define NOW 1792692433ull

static void assert_age(const char *want, uint64_t ago)
{
    char out[FEED_AGE_MAX];
    feedfmt_age(out, NOW - ago, NOW, true);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_feedfmt_age_steps_from_now_to_days(void)
{
    assert_age("now", 0);
    assert_age("now", 59);
    assert_age("1 min", 60);
    assert_age("59 min", 59 * 60 + 59);
    assert_age("1 h", 3600);
    assert_age("2 h", 2 * 3600 + 1800);
    assert_age("23 h", 86399);
    assert_age("1 d", 86400);
    assert_age("3 d", 3 * 86400);
    assert_age("999 d", 5000ull * 86400);
}

void test_feedfmt_age_is_empty_without_a_clock_or_an_at(void)
{
    char out[FEED_AGE_MAX] = "x";
    feedfmt_age(out, NOW - 300, NOW, false);
    TEST_ASSERT_EQUAL_STRING("", out);
    strcpy(out, "x");
    feedfmt_age(out, 0, NOW, true);
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_feedfmt_age_in_the_future_is_now(void)
{
    char out[FEED_AGE_MAX];
    feedfmt_age(out, NOW + 120, NOW, true);
    TEST_ASSERT_EQUAL_STRING("now", out);
}

static void assert_rows(size_t cards, size_t notes, size_t want_rows, size_t want_more)
{
    size_t more = 99;
    TEST_ASSERT_EQUAL_size_t(want_rows, feedfmt_note_rows(cards, notes, &more));
    TEST_ASSERT_EQUAL_size_t(want_more, more);
}

void test_feedfmt_rows_without_cards_fit_nine(void)
{
    assert_rows(0, 0, 0, 0);
    assert_rows(0, 9, 9, 0);
    assert_rows(0, 10, 9, 1); // the "+N more" line takes the 10th row's space, not a 9th note's
    assert_rows(0, 12, 9, 3);
}

void test_feedfmt_rows_under_four_cards_fit_seven(void)
{
    assert_rows(1, 7, 7, 0);
    assert_rows(4, 7, 7, 0);
    assert_rows(4, 8, 7, 1); // 7 rows and the "+1 more" line still fit
}

void test_feedfmt_rows_under_eight_cards_fit_five(void)
{
    assert_rows(5, 5, 5, 0);
    assert_rows(8, 5, 5, 0);
    assert_rows(8, 6, 4, 2);
    assert_rows(8, 12, 4, 8);
}

void test_feedfmt_the_fullest_page_fits_the_area(void)
{
    // 8 cards, then the rows and the "+N more" line, never past the page.
    size_t more;
    const size_t rows = feedfmt_note_rows(8, 12, &more);
    const int used = FEED_PAD + 2 * FEED_CARD_H + FEED_CARD_GAP + FEED_PAD + (int)rows * FEED_NOTE_H + FEED_MORE_H;
    TEST_ASSERT_LESS_OR_EQUAL(FEED_AREA_H, used);
}

void test_feedfmt_run(void)
{
    RUN_TEST(test_feedfmt_age_steps_from_now_to_days);
    RUN_TEST(test_feedfmt_age_is_empty_without_a_clock_or_an_at);
    RUN_TEST(test_feedfmt_age_in_the_future_is_now);
    RUN_TEST(test_feedfmt_rows_without_cards_fit_nine);
    RUN_TEST(test_feedfmt_rows_under_four_cards_fit_seven);
    RUN_TEST(test_feedfmt_rows_under_eight_cards_fit_five);
    RUN_TEST(test_feedfmt_the_fullest_page_fits_the_area);
}
