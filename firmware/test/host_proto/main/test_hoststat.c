// Host tests for hoststat.c: the text of the status bar's host values.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hoststat.h"
#include "unity.h"

static char out[HOSTSTAT_GROUP_MAX];

static void rate_is(uint64_t bps, proto_net_unit_t unit, const char *want)
{
    hoststat_rate(out, sizeof(out), bps, unit);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

// Whole °C and the whole °F the bar shows for it. The same rows as TEMP_VECTORS in app/src-tauri/src/daemon/units.rs.
static const struct {
    int c;
    int f;
} temp_vectors[] = {
    {-40, -40}, {-37, -35}, {-3, 27},  {-1, 30},  {0, 32},    {37, 99},
    {64, 147},  {84, 183},  {85, 185}, {95, 203}, {100, 212}, {150, 302},
};

// Bytes per second, the unit and the text the bar shows. The same rows as RATE_VECTORS in units.rs.
static const struct {
    uint64_t bps;
    proto_net_unit_t unit;
    const char *text;
} rate_vectors[] = {
    {0, PROTO_NET_SI, "0"},
    {999, PROTO_NET_SI, "999"},
    {1000, PROTO_NET_SI, "1.0k"},
    {340000, PROTO_NET_SI, "340.0k"},
    {1250000, PROTO_NET_SI, "1.2M"},
    {999999999, PROTO_NET_SI, "999.9M"},
    {1000000000, PROTO_NET_SI, "1.0G"},
    {0, PROTO_NET_BINARY, "0"},
    {999, PROTO_NET_BINARY, "999"},
    {1000, PROTO_NET_BINARY, "0.9Ki"},
    {1024, PROTO_NET_BINARY, "1.0Ki"},
    {1024000, PROTO_NET_BINARY, "0.9Mi"},
    {1153434, PROTO_NET_BINARY, "1.1Mi"},
    {104857599, PROTO_NET_BINARY, "99.9Mi"},
    {104857600, PROTO_NET_BINARY, "100.0Mi"},
    {1048576000, PROTO_NET_BINARY, "0.9Gi"},
    {1073741824, PROTO_NET_BINARY, "1.0Gi"},
    {0, PROTO_NET_BITS, "0b"},
    {124, PROTO_NET_BITS, "992b"},
    {125, PROTO_NET_BITS, "1.0kb"},
    {1200000, PROTO_NET_BITS, "9.6Mb"},
    {125000000, PROTO_NET_BITS, "1.0Gb"},
    {12499999, PROTO_NET_BITS, "99.9Mb"},
};

static void clock_is(int64_t local, const char *want)
{
    hoststat_clock(out, sizeof(out), local);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_hoststat_percent(void)
{
    hoststat_percent(out, sizeof(out), 0);
    TEST_ASSERT_EQUAL_STRING("0%", out);
    hoststat_percent(out, sizeof(out), 12);
    TEST_ASSERT_EQUAL_STRING("12%", out);
    hoststat_percent(out, sizeof(out), 100);
    TEST_ASSERT_EQUAL_STRING("100%", out);
    hoststat_percent(out, sizeof(out), 250);
    TEST_ASSERT_EQUAL_STRING("100%", out);
}

void test_hoststat_temp_units(void)
{
    for (size_t i = 0; i < sizeof(temp_vectors) / sizeof(temp_vectors[0]); i++) {
        char want[8];
        hoststat_temp(out, sizeof(out), temp_vectors[i].c, PROTO_TEMP_C);
        snprintf(want, sizeof(want), "%d", temp_vectors[i].c);
        TEST_ASSERT_EQUAL_STRING(want, out);
        hoststat_temp(out, sizeof(out), temp_vectors[i].c, PROTO_TEMP_F);
        snprintf(want, sizeof(want), "%d", temp_vectors[i].f);
        TEST_ASSERT_EQUAL_STRING(want, out);
    }
    // Every whole °C on the wire fits the 3 cells in both units.
    for (int c = -40; c <= 150; c++) {
        hoststat_temp(out, sizeof(out), c, PROTO_TEMP_C);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_TEMP_W, (int)strlen(out));
        hoststat_temp(out, sizeof(out), c, PROTO_TEMP_F);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_TEMP_W, (int)strlen(out));
    }
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
    rate_is(0, PROTO_NET_SI, "0");
    rate_is(999, PROTO_NET_SI, "999");
    rate_is(1000, PROTO_NET_SI, "1.0k");
    rate_is(1023, PROTO_NET_SI, "1.0k");
    rate_is(1024, PROTO_NET_SI, "1.0k");
    rate_is(1234, PROTO_NET_SI, "1.2k");
    rate_is(340000, PROTO_NET_SI, "340.0k");
    rate_is(999999, PROTO_NET_SI, "999.9k"); // truncated, never "1000.0k"
    rate_is(1000000, PROTO_NET_SI, "1.0M");
    rate_is(1048575, PROTO_NET_SI, "1.0M");
    rate_is(1048576, PROTO_NET_SI, "1.0M");
    rate_is(12345678, PROTO_NET_SI, "12.3M");
    rate_is(999999999, PROTO_NET_SI, "999.9M");
    rate_is(2500000000ULL, PROTO_NET_SI, "2.5G");
    rate_is(999999999999ULL, PROTO_NET_SI, "999.9G");
    rate_is(UINT64_MAX, PROTO_NET_SI, "999.9G");
}

void test_hoststat_rate_binary_switches_prefix_at_1000(void)
{
    rate_is(1023, PROTO_NET_BINARY, "0.9Ki"); // 1000 KiB/s shows 0.9Mi, so no fourth integer digit is needed
    rate_is(999999, PROTO_NET_BINARY, "976.5Ki");
    rate_is(1000000, PROTO_NET_BINARY, "976.5Ki");
    rate_is(1048575, PROTO_NET_BINARY, "0.9Mi");
    rate_is(1048576, PROTO_NET_BINARY, "1.0Mi");
    rate_is(999999999, PROTO_NET_BINARY, "953.6Mi");
    rate_is(2500000000ULL, PROTO_NET_BINARY, "2.3Gi");
    rate_is(UINT64_MAX, PROTO_NET_BINARY, "999.9Gi");
}

void test_hoststat_rate_bits_are_bytes_times_8(void)
{
    rate_is(999, PROTO_NET_BITS, "7.9kb");
    rate_is(1000, PROTO_NET_BITS, "8.0kb");
    rate_is(1023, PROTO_NET_BITS, "8.1kb");
    rate_is(1024, PROTO_NET_BITS, "8.1kb");
    rate_is(999999, PROTO_NET_BITS, "7.9Mb");
    rate_is(1000000, PROTO_NET_BITS, "8.0Mb");
    rate_is(1048575, PROTO_NET_BITS, "8.3Mb");
    rate_is(1048576, PROTO_NET_BITS, "8.3Mb");
    rate_is(999999999, PROTO_NET_BITS, "7.9Gb");
    rate_is(2500000000ULL, PROTO_NET_BITS, "20.0Gb");
    rate_is(UINT64_MAX, PROTO_NET_BITS, "999.9Gb"); // bytes * 8 would overflow
}

void test_hoststat_rate_vectors_match_the_daemon(void)
{
    for (size_t i = 0; i < sizeof(rate_vectors) / sizeof(rate_vectors[0]); i++) {
        rate_is(rate_vectors[i].bps, rate_vectors[i].unit, rate_vectors[i].text);
    }
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

// The bar pads every percent to HOSTSTAT_VALUE_W cells and every rate to its unit's width, so none may be longer.
void test_hoststat_values_fit_the_bar_cells(void)
{
    TEST_ASSERT_EQUAL_INT(6, hoststat_rate_width(PROTO_NET_SI));
    TEST_ASSERT_EQUAL_INT(7, hoststat_rate_width(PROTO_NET_BINARY));
    TEST_ASSERT_EQUAL_INT(7, hoststat_rate_width(PROTO_NET_BITS));
    for (unsigned cpu = 0; cpu <= 100; cpu++) {
        hoststat_percent(out, sizeof(out), cpu);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_VALUE_W, (int)strlen(out));
    }
    for (uint64_t used = 0; used <= 1000; used += 7) {
        hoststat_mem(out, sizeof(out), used, 1000);
        TEST_ASSERT_LESS_OR_EQUAL_INT(HOSTSTAT_VALUE_W, (int)strlen(out));
    }
    for (proto_net_unit_t unit = PROTO_NET_SI; unit <= PROTO_NET_BITS; unit++) {
        for (uint64_t bps = 1; bps < UINT64_MAX / 3; bps *= 3) {
            hoststat_rate(out, sizeof(out), bps, unit);
            TEST_ASSERT_LESS_OR_EQUAL_INT(hoststat_rate_width(unit), (int)strlen(out));
        }
        hoststat_rate(out, sizeof(out), UINT64_MAX, unit);
        TEST_ASSERT_LESS_OR_EQUAL_INT(hoststat_rate_width(unit), (int)strlen(out));
    }
}

#define RED 0xe53935
#define AMBER 0xffb300

static void color_on(proto_stats_t *stats, proto_color_t which, uint32_t rgb)
{
    stats->colors[which] = rgb;
    stats->colors_mask |= 1u << which;
}

static void group_is(void (*group)(char *, size_t, const proto_stats_t *, bool), const proto_stats_t *stats, bool fresh,
                     const char *want)
{
    group(out, sizeof(out), stats, fresh);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

static void net_is(const proto_stats_t *stats, bool fresh, const char *want)
{
    hoststat_group_net(out, sizeof(out), stats, fresh, "v", "^");
    TEST_ASSERT_EQUAL_STRING(want, out);
}

void test_hoststat_cpu_group_rows(void)
{
    proto_stats_t s = {0};
    group_is(hoststat_group_cpu, &s, true,
             "CPU   --  --\xC2\xB0"
             "C"); // neither value
    s.has_cpu = true;
    s.cpu = 12;
    group_is(hoststat_group_cpu, &s, true,
             "CPU  12%  --\xC2\xB0"
             "C"); // busy only
    s.has_cpu = false;
    s.has_cpu_temp = true;
    s.cpu_temp = 64;
    group_is(hoststat_group_cpu, &s, true,
             "CPU   --  64\xC2\xB0"
             "C"); // temperature only
    s.has_cpu = true;
    group_is(hoststat_group_cpu, &s, true,
             "CPU  12%  64\xC2\xB0"
             "C"); // both
    s.cpu = 100;
    s.cpu_temp = -40;
    group_is(hoststat_group_cpu, &s, true,
             "CPU 100% -40\xC2\xB0"
             "C");
    s.cpu = 7;
    s.cpu_temp = 150;
    s.temp_unit = PROTO_TEMP_F;
    group_is(hoststat_group_cpu, &s, true,
             "CPU   7% 302\xC2\xB0"
             "F"); // the letter follows the unit
    s.cpu_temp = 64;
    group_is(hoststat_group_cpu, &s, true,
             "CPU   7% 147\xC2\xB0"
             "F");
}

void test_hoststat_gpu_group_rows(void)
{
    proto_stats_t s = {0};
    group_is(hoststat_group_gpu, &s, true,
             "GPU   --  --\xC2\xB0"
             "C");
    s.has_gpu = true;
    s.gpu = 23;
    group_is(hoststat_group_gpu, &s, true,
             "GPU  23%  --\xC2\xB0"
             "C"); // a GPU without a sensor still shows busy
    s.has_gpu_temp = true;
    s.gpu_temp = 34;
    group_is(hoststat_group_gpu, &s, true,
             "GPU  23%  34\xC2\xB0"
             "C");
    s.has_gpu = false;
    group_is(hoststat_group_gpu, &s, true,
             "GPU   --  34\xC2\xB0"
             "C");
}

void test_hoststat_mem_group(void)
{
    proto_stats_t s = {0};
    group_is(hoststat_group_mem, &s, true, "RAM   --");
    s.has_mem = true;
    s.mem_total = 10000;
    s.mem_used = 8999; // 89.99%
    group_is(hoststat_group_mem, &s, true, "RAM  89%");
    s.mem_used = 10000;
    group_is(hoststat_group_mem, &s, true, "RAM 100%");
    s.mem_total = 0;
    color_on(&s, PROTO_COLOR_MEM, RED);
    group_is(hoststat_group_mem, &s, true, "RAM   --"); // "--" is never colored
}

void test_hoststat_net_group_cells_per_unit(void)
{
    proto_stats_t s = {.has_net = true, .net_rx = 1250000, .net_tx = 340000};
    net_is(&s, true, "v  1.2M ^340.0k");
    s.net_unit = PROTO_NET_BINARY;
    net_is(&s, true, "v  1.1Mi ^332.0Ki");
    s.net_unit = PROTO_NET_BITS;
    net_is(&s, true, "v 10.0Mb ^  2.7Mb");
    s.has_net = false;
    net_is(&s, true, "v     -- ^     --");
}

void test_hoststat_colors_wrap_exactly_the_value(void)
{
    proto_stats_t s = {.has_cpu = true, .cpu = 97, .has_cpu_temp = true, .cpu_temp = 64};
    color_on(&s, PROTO_COLOR_CPU, RED);
    group_is(hoststat_group_cpu, &s, true,
             "CPU  #e53935 97%#  64\xC2\xB0"
             "C"); // the padding stays outside
    color_on(&s, PROTO_COLOR_CPU_TEMP, AMBER);
    group_is(hoststat_group_cpu, &s, true,
             "CPU  #e53935 97%#  #ffb300 64#\xC2\xB0"
             "C"); // the unit stays grey, too
    s.colors_mask = 1u << PROTO_COLOR_CPU_TEMP;
    group_is(hoststat_group_cpu, &s, true,
             "CPU  97%  #ffb300 64#\xC2\xB0"
             "C");

    proto_stats_t g = {.has_gpu = true, .gpu = 100};
    color_on(&g, PROTO_COLOR_GPU, AMBER);
    group_is(hoststat_group_gpu, &g, true,
             "GPU #ffb300 100%#  --\xC2\xB0"
             "C");

    proto_stats_t m = {.has_mem = true, .mem_used = 9, .mem_total = 10};
    color_on(&m, PROTO_COLOR_MEM, RED);
    group_is(hoststat_group_mem, &m, true, "RAM  #e53935 90%#");

    proto_stats_t n = {.has_net = true, .net_rx = 1250000, .net_tx = 340000};
    color_on(&n, PROTO_COLOR_NET_RX, RED);
    net_is(&n, true, "v  #e53935 1.2M# ^340.0k");
    color_on(&n, PROTO_COLOR_NET_TX, AMBER);
    net_is(&n, true, "v  #e53935 1.2M# ^#ffb300 340.0k#");
    n.colors_mask = 1u << PROTO_COLOR_NET_TX;
    net_is(&n, true, "v  1.2M ^#ffb300 340.0k#");
}

void test_hoststat_color_of_an_absent_value_adds_nothing(void)
{
    proto_stats_t s = {0};
    s.colors_mask = (1u << PROTO_COLOR_COUNT) - 1;
    for (int i = 0; i < PROTO_COLOR_COUNT; i++) {
        s.colors[i] = RED;
    }
    group_is(hoststat_group_cpu, &s, true,
             "CPU   --  --\xC2\xB0"
             "C");
    group_is(hoststat_group_gpu, &s, true,
             "GPU   --  --\xC2\xB0"
             "C");
    group_is(hoststat_group_mem, &s, true, "RAM   --");
    net_is(&s, true, "v    -- ^    --");
    // A busy value without its temperature colors only the busy value.
    s.has_cpu = true;
    s.cpu = 50;
    group_is(hoststat_group_cpu, &s, true,
             "CPU  #e53935 50%#  --\xC2\xB0"
             "C");
}

void test_hoststat_stale_shows_dashes_with_no_markup(void)
{
    proto_stats_t s = {
        .has_cpu = true,
        .cpu = 97,
        .has_cpu_temp = true,
        .cpu_temp = 64,
        .has_gpu = true,
        .gpu = 5,
        .has_gpu_temp = true,
        .gpu_temp = 40,
        .has_mem = true,
        .mem_used = 9,
        .mem_total = 10,
        .has_net = true,
        .net_rx = 1,
        .net_tx = 2,
        .temp_unit = PROTO_TEMP_F,
        .net_unit = PROTO_NET_BINARY,
    };
    s.colors_mask = (1u << PROTO_COLOR_COUNT) - 1;
    for (int i = 0; i < PROTO_COLOR_COUNT; i++) {
        s.colors[i] = RED;
    }
    group_is(hoststat_group_cpu, &s, false,
             "CPU   --  --\xC2\xB0"
             "F");
    group_is(hoststat_group_gpu, &s, false,
             "GPU   --  --\xC2\xB0"
             "F");
    group_is(hoststat_group_mem, &s, false, "RAM   --");
    net_is(&s, false, "v     -- ^     --");
}

// Every group, in every unit and with a color on every value, fits the buffer.
void test_hoststat_groups_fit_their_buffer(void)
{
    proto_stats_t s = {
        .has_cpu = true,
        .cpu = 100,
        .has_cpu_temp = true,
        .cpu_temp = 150,
        .has_gpu = true,
        .gpu = 100,
        .has_gpu_temp = true,
        .gpu_temp = 150,
        .has_mem = true,
        .mem_used = 1,
        .mem_total = 1,
        .has_net = true,
        .net_rx = UINT64_MAX,
        .net_tx = UINT64_MAX,
    };
    for (int t = PROTO_TEMP_C; t <= PROTO_TEMP_F; t++) {
        for (int n = PROTO_NET_SI; n <= PROTO_NET_BITS; n++) {
            s.temp_unit = t;
            s.net_unit = n;
            s.colors_mask = (1u << PROTO_COLOR_COUNT) - 1;
            for (int i = 0; i < PROTO_COLOR_COUNT; i++) {
                s.colors[i] = 0xffffff;
            }
            char colored[HOSTSTAT_GROUP_MAX];
            hoststat_group_net(colored, sizeof(colored), &s, true, "v", "^");
            TEST_ASSERT_LESS_THAN_INT(HOSTSTAT_GROUP_MAX - 1, (int)strlen(colored));
            hoststat_group_cpu(colored, sizeof(colored), &s, true);
            TEST_ASSERT_LESS_THAN_INT(HOSTSTAT_GROUP_MAX - 1, (int)strlen(colored));
            hoststat_group_gpu(colored, sizeof(colored), &s, true);
            TEST_ASSERT_LESS_THAN_INT(HOSTSTAT_GROUP_MAX - 1, (int)strlen(colored));
        }
    }
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
    RUN_TEST(test_hoststat_percent);
    RUN_TEST(test_hoststat_temp_units);
    RUN_TEST(test_hoststat_mem_percent_used);
    RUN_TEST(test_hoststat_rate_si_prefix_truncates);
    RUN_TEST(test_hoststat_rate_binary_switches_prefix_at_1000);
    RUN_TEST(test_hoststat_rate_bits_are_bytes_times_8);
    RUN_TEST(test_hoststat_rate_vectors_match_the_daemon);
    RUN_TEST(test_hoststat_cpu_group_rows);
    RUN_TEST(test_hoststat_gpu_group_rows);
    RUN_TEST(test_hoststat_mem_group);
    RUN_TEST(test_hoststat_net_group_cells_per_unit);
    RUN_TEST(test_hoststat_colors_wrap_exactly_the_value);
    RUN_TEST(test_hoststat_color_of_an_absent_value_adds_nothing);
    RUN_TEST(test_hoststat_stale_shows_dashes_with_no_markup);
    RUN_TEST(test_hoststat_groups_fit_their_buffer);
    RUN_TEST(test_hoststat_clock_from_gmtime);
}
