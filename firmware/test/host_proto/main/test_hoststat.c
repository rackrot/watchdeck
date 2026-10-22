// Host tests for hoststat.c: the text of the status bar's host values.
#include <stdint.h>
#include <string.h>

#include "hoststat.h"
#include "unity.h"

static char out[HOSTSTAT_TEXT_MAX];

static void rate_is(uint64_t bps, const char *want)
{
    hoststat_rate(out, sizeof(out), bps);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void clock_is(int64_t local, const char *want)
{
    hoststat_clock(out, sizeof(out), local);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_hoststat_cpu_percent(void)
{
    hoststat_cpu(out, sizeof(out), 0);
    TEST_ASSERT_EQUAL_STRING("0%", out);
    hoststat_cpu(out, sizeof(out), 12);
    TEST_ASSERT_EQUAL_STRING("12%", out);
    hoststat_cpu(out, sizeof(out), 100);
    TEST_ASSERT_EQUAL_STRING("100%", out);
    hoststat_cpu(out, sizeof(out), 250);
    TEST_ASSERT_EQUAL_STRING("100%", out);
}

void test_hoststat_mem_percent_used(void)
{
    hoststat_mem(out, sizeof(out), 10522394624ULL, 33395638272ULL); // 9.8 of 31.1 GiB
    TEST_ASSERT_EQUAL_STRING("31%", out);
    hoststat_mem(out, sizeof(out), 0, 1000);
    TEST_ASSERT_EQUAL_STRING("0%", out);
    hoststat_mem(out, sizeof(out), 999, 1000);
    TEST_ASSERT_EQUAL_STRING("99%", out); // truncated, never rounded up to a false 100
    hoststat_mem(out, sizeof(out), 1000, 1000);
    TEST_ASSERT_EQUAL_STRING("100%", out);
    hoststat_mem(out, sizeof(out), 2000, 1000);
    TEST_ASSERT_EQUAL_STRING("100%", out);
    hoststat_mem(out, sizeof(out), 1ULL << 40, 2ULL << 40); // a total over 1 TiB
    TEST_ASSERT_EQUAL_STRING("50%", out);
    hoststat_mem(out, sizeof(out), 5, 0);
    TEST_ASSERT_EQUAL_STRING("--", out);
    hoststat_mem(out, sizeof(out), UINT64_MAX / 2, UINT64_MAX); // no overflow in used * 100
    TEST_ASSERT_EQUAL_STRING("50%", out);
}

void test_hoststat_rate_si_prefix_truncates(void)
{
    rate_is(0, "0");
    rate_is(999, "999");
    rate_is(1000, "1.0k");
    rate_is(1234, "1.2k");
    rate_is(340000, "340.0k");
    rate_is(999999, "999.9k"); // truncated, never "1000.0k"
    rate_is(1000000, "1.0M");
    rate_is(12345678, "12.3M");
    rate_is(999999999, "999.9M");
    rate_is(2500000000ULL, "2.5G");
    rate_is(999999999999ULL, "999.9G");
    rate_is(UINT64_MAX, "999.9G");
}

void test_hoststat_clock_from_gmtime(void)
{
    clock_is(0, "Thu 01 Jan 00:00");
    clock_is(1709208000, "Thu 29 Feb 12:00");            // a leap day
    clock_is(1792634400 - 4 * 3600, "Wed 21 Oct 22:00"); // 02:00 UTC at -04:00 is the evening before
    clock_is(-3600, "Wed 31 Dec 23:00");                 // before the epoch
    clock_is(2147483648LL, "Tue 19 Jan 03:14");          // past 2^31
    clock_is(2208988800LL, "Sun 01 Jan 00:00");          // 2040
    clock_is(INT64_MAX, "--:--");
}

// The bar pads every percent to HOSTSTAT_VALUE_W cells and every rate to HOSTSTAT_RATE_W, so none may be longer.
void test_hoststat_values_fit_the_bar_cells(void)
{
    for (unsigned cpu = 0; cpu <= 100; cpu++) {
        hoststat_cpu(out, sizeof(out), cpu);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_VALUE_W, (int)strlen(out));
    }
    for (uint64_t used = 0; used <= 1000; used += 7) {
        hoststat_mem(out, sizeof(out), used, 1000);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_VALUE_W, (int)strlen(out));
    }
    for (uint64_t bps = 1; bps < UINT64_MAX / 3; bps *= 3) {
        hoststat_rate(out, sizeof(out), bps);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_RATE_W, (int)strlen(out));
    }
    hoststat_rate(out, sizeof(out), UINT64_MAX);
    TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_RATE_W, (int)strlen(out));
}

void test_hoststat_fresh_until_5_s(void)
{
    TEST_ASSERT_TRUE(hoststat_fresh(true, 0));
    TEST_ASSERT_TRUE(hoststat_fresh(true, 5000000)); // exactly 5 s is still fresh
    TEST_ASSERT_FALSE(hoststat_fresh(true, 5000001));
    TEST_ASSERT_FALSE(hoststat_fresh(false, 0)); // never had a frame
}

void test_hoststat_local_time_runs_on_from_the_frame(void)
{
    TEST_ASSERT_EQUAL_INT64(1792627170, hoststat_local_time(1792627170, 0, 999999)); // whole seconds only
    TEST_ASSERT_EQUAL_INT64(1792627171, hoststat_local_time(1792627170, 0, 1000000));
    // 23:59:30 UTC on 21 Oct plus 45 s is past midnight.
    clock_is(hoststat_local_time(1792627170, 0, 0), "Wed 21 Oct 23:59");
    clock_is(hoststat_local_time(1792627170, 0, 45 * 1000000LL), "Thu 22 Oct 00:00");
    // A negative offset: 02:00:00 UTC at -04:00 is 22:00 the evening before, and 2 h later is midnight.
    clock_is(hoststat_local_time(1792634400, -4 * 3600, 0), "Wed 21 Oct 22:00");
    clock_is(hoststat_local_time(1792634400, -4 * 3600, 2 * 3600 * 1000000LL), "Thu 22 Oct 00:00");
}

void test_hoststat_run(void)
{
    RUN_TEST(test_hoststat_fresh_until_5_s);
    RUN_TEST(test_hoststat_local_time_runs_on_from_the_frame);
    RUN_TEST(test_hoststat_values_fit_the_bar_cells);
    RUN_TEST(test_hoststat_cpu_percent);
    RUN_TEST(test_hoststat_mem_percent_used);
    RUN_TEST(test_hoststat_rate_si_prefix_truncates);
    RUN_TEST(test_hoststat_clock_from_gmtime);
}
