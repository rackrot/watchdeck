// Host tests for pageset.c: how the board assembles a set of layout pages (site/src/protocol.md, Pages).
#include <stdio.h>
#include <string.h>

#include "pageset.h"
#include "unity.h"

static proto_layout_t slots[PROTO_MAX_PAGES];
static pageset_t ps;
static const char *why;

// The page under test, filled by page_of or plain. Static, since a layout is about 6 KB and the test task's stack is
// small.
static proto_layout_t in;

// A page of a set, with its rev and index stamped into its first button so a test can tell the pages apart.
static const proto_layout_t *page_of(uint32_t rev, uint8_t page, uint8_t pages)
{
    memset(&in, 0, sizeof(in));
    in.rev = rev;
    in.page = page;
    in.pages = pages;
    in.cols = 5;
    in.rows = 3;
    in.n_buttons = 1;
    snprintf(in.page_id, sizeof(in.page_id), "p%u", page);
    snprintf(in.buttons[0].id, sizeof(in.buttons[0].id), "b%u-%u", (unsigned)rev, page);
    return &in;
}

// A layout without page fields, as proto_parse gives it: page 0 of 1 with an empty id.
static const proto_layout_t *plain(uint32_t rev)
{
    page_of(rev, 0, 1);
    in.page_id[0] = '\0';
    return &in;
}

static pageset_result_t add(const proto_layout_t *l, int64_t now_ms)
{
    return pageset_add(&ps, l, now_ms, &why);
}

static void fresh(void)
{
    memset(slots, 0, sizeof(slots));
    ps.pages = slots;
    pageset_reset(&ps);
}

void test_pageset_complete_set_of_4(void)
{
    fresh();
    for (uint8_t p = 0; p < 3; p++) {
        TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(7, p, 4), 100));
        TEST_ASSERT_TRUE(pageset_arriving(&ps));
    }
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(page_of(7, 3, 4), 200));
    TEST_ASSERT_NULL(why);
    TEST_ASSERT_FALSE(pageset_arriving(&ps));
    TEST_ASSERT_EQUAL_UINT32(7, ps.rev);
    TEST_ASSERT_EQUAL_UINT8(4, ps.n_pages);
    for (uint8_t p = 0; p < 4; p++) {
        char want[16];
        snprintf(want, sizeof(want), "b7-%u", p);
        TEST_ASSERT_EQUAL_STRING(want, ps.pages[p].buttons[0].id);
    }
}

void test_pageset_plain_layout_is_a_set_of_one(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(plain(1), 0));
    TEST_ASSERT_EQUAL_UINT8(1, ps.n_pages);
    TEST_ASSERT_EQUAL_STRING("", ps.pages[0].page_id);
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(plain(2), 0));
    TEST_ASSERT_EQUAL_UINT32(2, ps.rev);
}

void test_pageset_missing_middle_page_breaks_at_once(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(3, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(3, 2, 3), 0));
    TEST_ASSERT_EQUAL_STRING("a page out of order", why);
    TEST_ASSERT_FALSE(pageset_arriving(&ps));
}

void test_pageset_missing_first_page_breaks(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(3, 1, 3), 0));
    TEST_ASSERT_EQUAL_STRING("a set that doesn't start at page 0", why);
}

// Only the 5 s timer catches a lost last page, since nothing else comes until the next push.
void test_pageset_missing_last_page_expires_after_5_s(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(3, 0, 3), 1000));
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(3, 1, 3), 1200));
    TEST_ASSERT_TRUE(pageset_arriving(&ps));
    TEST_ASSERT_FALSE(pageset_expired(&ps, 1000 + PAGESET_TIMEOUT_MS - 1));
    TEST_ASSERT_TRUE(pageset_expired(&ps, 1000 + PAGESET_TIMEOUT_MS));
}

void test_pageset_nothing_arriving_never_expires(void)
{
    fresh();
    TEST_ASSERT_FALSE(pageset_expired(&ps, 1000000));
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(plain(1), 0));
    TEST_ASSERT_FALSE(pageset_expired(&ps, 1000000));
}

void test_pageset_count_change_mid_set_breaks(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(4, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(4, 1, 2), 0));
    TEST_ASSERT_EQUAL_STRING("page counts disagree", why);
}

void test_pageset_newer_rev_mid_set_discards_it(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(4, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(4, 1, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(5, 0, 2), 3000));
    TEST_ASSERT_EQUAL_STRING("a newer rev discarded the set arriving", why);
    TEST_ASSERT_EQUAL_UINT32(5, ps.rev);
    // The timer restarts with the new set's first page.
    TEST_ASSERT_FALSE(pageset_expired(&ps, 3000 + PAGESET_TIMEOUT_MS - 1));
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(page_of(5, 1, 2), 3100));
    TEST_ASSERT_EQUAL_UINT8(2, ps.n_pages);
    TEST_ASSERT_EQUAL_STRING("b5-0", ps.pages[0].buttons[0].id);
    TEST_ASSERT_EQUAL_STRING("b5-1", ps.pages[1].buttons[0].id);
}

void test_pageset_newer_rev_mid_set_not_at_page_0_breaks(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(4, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(5, 1, 3), 0));
}

void test_pageset_duplicate_page_breaks(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(6, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(6, 1, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(6, 1, 3), 0));
    TEST_ASSERT_EQUAL_STRING("a page came twice", why);
}

void test_pageset_older_rev_after_a_newer_one_breaks(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(8, 0, 2), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(7, 1, 2), 0));
    TEST_ASSERT_EQUAL_STRING("a page of an older rev", why);

    // After a complete set, too, and for the same rev again.
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(8, 0, 2), 0));
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(page_of(8, 1, 2), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(page_of(7, 0, 2), 0));
    TEST_ASSERT_EQUAL_STRING("a rev no newer than the last set", why);
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(plain(8), 0));
}

void test_pageset_plain_layout_while_a_set_is_arriving(void)
{
    // Of the same rev, it disagrees with the set's page count.
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(2, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_BROKEN, add(plain(2), 0));
    TEST_ASSERT_EQUAL_STRING("page counts disagree", why);

    // Of a newer rev, it discards the set and is a complete set of one.
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(2, 0, 3), 0));
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(plain(3), 0));
    TEST_ASSERT_EQUAL_STRING("a newer rev discarded the set arriving", why);
    TEST_ASSERT_EQUAL_UINT8(1, ps.n_pages);
    TEST_ASSERT_EQUAL_STRING("b3-0", ps.pages[0].buttons[0].id);
}

// Every connection starts again at rev 1, so a reset forgets the last rev and any set arriving.
void test_pageset_reset_starts_a_new_connection(void)
{
    fresh();
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(plain(9), 0));
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(10, 0, 2), 0));
    pageset_reset(&ps);
    TEST_ASSERT_FALSE(pageset_arriving(&ps));
    TEST_ASSERT_EQUAL_PTR(slots, ps.pages);
    TEST_ASSERT_EQUAL(PAGESET_PENDING, add(page_of(1, 0, 2), 0));
    TEST_ASSERT_EQUAL(PAGESET_COMPLETE, add(page_of(1, 1, 2), 0));
}

void test_pageset_run(void)
{
    RUN_TEST(test_pageset_complete_set_of_4);
    RUN_TEST(test_pageset_plain_layout_is_a_set_of_one);
    RUN_TEST(test_pageset_missing_middle_page_breaks_at_once);
    RUN_TEST(test_pageset_missing_first_page_breaks);
    RUN_TEST(test_pageset_missing_last_page_expires_after_5_s);
    RUN_TEST(test_pageset_nothing_arriving_never_expires);
    RUN_TEST(test_pageset_count_change_mid_set_breaks);
    RUN_TEST(test_pageset_newer_rev_mid_set_discards_it);
    RUN_TEST(test_pageset_newer_rev_mid_set_not_at_page_0_breaks);
    RUN_TEST(test_pageset_duplicate_page_breaks);
    RUN_TEST(test_pageset_older_rev_after_a_newer_one_breaks);
    RUN_TEST(test_pageset_plain_layout_while_a_set_is_arriving);
    RUN_TEST(test_pageset_reset_starts_a_new_connection);
}
