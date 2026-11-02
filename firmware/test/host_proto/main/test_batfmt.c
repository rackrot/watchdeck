// The battery monitor's decoding, freshness and send rule (batfmt.c).
#include <string.h>

#include "batfmt.h"
#include "unity.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void regs_of(uint8_t regs[BATFMT_REGS], uint32_t pin, uint32_t mv, uint8_t percent, uint8_t status)
{
    memset(regs, 0, BATFMT_REGS);
    put32(regs, pin);
    put32(regs + 4, mv);
    regs[8] = percent;
    regs[9] = status;
    regs[10] = status;
}

static bat_reading_t reading(uint8_t percent, bat_charge_t charge)
{
    bat_reading_t r = {.pin_mv = 2787, .mv = 3873, .percent = percent, .charge = charge};
    return r;
}

void test_batfmt_decodes_the_two_probed_states(void)
{
    uint8_t regs[BATFMT_REGS];
    bat_reading_t r;

    regs_of(regs, 2948, 4097, 85, 0x01);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    TEST_ASSERT_EQUAL_UINT32(2948, r.pin_mv);
    TEST_ASSERT_EQUAL_UINT32(4097, r.mv);
    TEST_ASSERT_EQUAL_UINT8(85, r.percent);
    TEST_ASSERT_EQUAL_UINT8(0x01, r.status);
    TEST_ASSERT_EQUAL(BAT_CHARGING, r.charge);

    regs_of(regs, 2787, 3873, 53, 0x03);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    TEST_ASSERT_EQUAL_UINT32(3873, r.mv);
    TEST_ASSERT_EQUAL_UINT8(53, r.percent);
    TEST_ASSERT_EQUAL(BAT_DISCHARGING, r.charge);
}

void test_batfmt_decodes_little_endian_from_the_raw_bytes(void)
{
    // 2787 = 0x0AE3 and 3873 = 0x0F21, as they come off the bus.
    const uint8_t regs[BATFMT_REGS] = {0xE3, 0x0A, 0x00, 0x00, 0x21, 0x0F, 0x00, 0x00, 53, 0x03, 0x03};
    bat_reading_t r;
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    TEST_ASSERT_EQUAL_UINT32(2787, r.pin_mv);
    TEST_ASSERT_EQUAL_UINT32(3873, r.mv);
}

void test_batfmt_maps_each_status_byte(void)
{
    const bat_charge_t want[4] = {BAT_UNKNOWN, BAT_CHARGING, BAT_FULL, BAT_DISCHARGING};
    for (uint8_t s = 0; s < 4; s++) {
        uint8_t regs[BATFMT_REGS];
        bat_reading_t r;
        regs_of(regs, 2787, 3873, 53, s);
        TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
        TEST_ASSERT_EQUAL(want[s], r.charge);
        TEST_ASSERT_EQUAL_UINT8(s, r.status);
    }
}

void test_batfmt_refuses_junk_and_leaves_out_alone(void)
{
    uint8_t regs[BATFMT_REGS];
    bat_reading_t r = reading(7, BAT_FULL);
    const bat_reading_t before = r;

    memset(regs, 0x00, sizeof(regs));
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    memset(regs, 0xFF, sizeof(regs));
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3000, 53, 0x03); // the battery doesn't follow the pin
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873, 101, 0x03);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873, 53, 0x04);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873, 53, 0x80);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    TEST_ASSERT_EQUAL_MEMORY(&before, &r, sizeof(r));
}

void test_batfmt_check_does_not_overflow_in_32_bits(void)
{
    uint8_t regs[BATFMT_REGS];
    bat_reading_t r;
    // 30899046 * 139 = 2^32 + 98: a 32-bit check expects 0 mV and would accept a battery of 0 mV.
    regs_of(regs, 30899046u, 0, 50, 0x03);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
}

void test_batfmt_cap_and_tolerance_edges(void)
{
    uint8_t regs[BATFMT_REGS];
    bat_reading_t r;
    // Over the 6000 mV cap, even though it matches the pin exactly.
    regs_of(regs, 4400, 6116, 50, 0x03);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    // 2787 x 139 / 100 = 3873; the tolerance is 2 % + 20 = 97 mV.
    regs_of(regs, 2787, 3873 + 97, 50, 0x03);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873 + 98, 50, 0x03);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873 - 97, 50, 0x03);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873 - 98, 50, 0x03);
    TEST_ASSERT_FALSE(batfmt_decode(regs, &r));
    // 0 and 100 percent are readings.
    regs_of(regs, 2787, 3873, 0, 0x03);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
    regs_of(regs, 2787, 3873, 100, 0x03);
    TEST_ASSERT_TRUE(batfmt_decode(regs, &r));
}

void test_batfmt_fresh_until_exactly_fifteen_seconds(void)
{
    TEST_ASSERT_TRUE(batfmt_fresh(true, 0));
    TEST_ASSERT_TRUE(batfmt_fresh(true, 15 * 1000 * 1000LL));
    TEST_ASSERT_FALSE(batfmt_fresh(true, 15 * 1000 * 1000LL + 1));
    TEST_ASSERT_FALSE(batfmt_fresh(false, 0));
}

void test_batfmt_level_is_monotonic_and_spans_the_range(void)
{
    int last = batfmt_level(0);
    TEST_ASSERT_EQUAL(0, last);
    for (unsigned p = 1; p <= 100; p++) {
        const int l = batfmt_level(p);
        TEST_ASSERT_TRUE(l >= last);
        TEST_ASSERT_TRUE(l < BAT_LEVELS);
        last = l;
    }
    TEST_ASSERT_EQUAL(BAT_LEVELS - 1, batfmt_level(100));
    TEST_ASSERT_EQUAL(BAT_LEVELS - 1, batfmt_level(250));
}

void test_batfmt_sends_the_first_frame(void)
{
    const bat_reading_t now = reading(53, BAT_DISCHARGING);
    TEST_ASSERT_TRUE(batfmt_should_send(false, NULL, &now));
    TEST_ASSERT_TRUE(batfmt_should_send(false, NULL, NULL));
}

void test_batfmt_sends_a_charge_change_alone(void)
{
    const bat_reading_t sent = reading(53, BAT_DISCHARGING);
    const bat_reading_t now = reading(53, BAT_CHARGING);
    TEST_ASSERT_TRUE(batfmt_should_send(true, &sent, &now));
}

void test_batfmt_ignores_a_one_point_move(void)
{
    const bat_reading_t sent = reading(53, BAT_DISCHARGING);
    const bat_reading_t up = reading(54, BAT_DISCHARGING);
    const bat_reading_t down = reading(52, BAT_DISCHARGING);
    const bat_reading_t same = reading(53, BAT_DISCHARGING);
    TEST_ASSERT_FALSE(batfmt_should_send(true, &sent, &up));
    TEST_ASSERT_FALSE(batfmt_should_send(true, &sent, &down));
    TEST_ASSERT_FALSE(batfmt_should_send(true, &sent, &same));
    TEST_ASSERT_FALSE(batfmt_should_send(true, NULL, NULL));
}

void test_batfmt_measures_a_two_point_move_from_the_last_sent(void)
{
    // 53 sent, then 54 seen but not sent: 55 is 2 from 53 and goes, though it is 1 from 54.
    const bat_reading_t sent = reading(53, BAT_DISCHARGING);
    const bat_reading_t seen = reading(54, BAT_DISCHARGING);
    const bat_reading_t now = reading(55, BAT_DISCHARGING);
    const bat_reading_t down = reading(51, BAT_DISCHARGING);
    TEST_ASSERT_FALSE(batfmt_should_send(true, &sent, &seen));
    TEST_ASSERT_TRUE(batfmt_should_send(true, &sent, &now));
    TEST_ASSERT_TRUE(batfmt_should_send(true, &sent, &down));
}

void test_batfmt_sends_when_a_reading_comes_and_goes(void)
{
    const bat_reading_t r = reading(53, BAT_DISCHARGING);
    const bat_reading_t same = reading(53, BAT_DISCHARGING); // the very reading sent before it went away
    TEST_ASSERT_TRUE(batfmt_should_send(true, &r, NULL));
    TEST_ASSERT_TRUE(batfmt_should_send(true, NULL, &same));
}

void test_batfmt_text_fits_the_buffers_and_reports_a_cut(void)
{
    // Not the wording: the worst cases fit their buffers, and a too-small buffer is reported.
    const bat_reading_t r = {.pin_mv = 2948, .mv = 4100, .percent = 100, .charge = BAT_DISCHARGING};
    const bat_reading_t none = {.mv = 10000, .percent = 100, .charge = BAT_NONE};
    char pct[BATFMT_PERCENT_MAX];
    char row[BATFMT_ROW_MAX];
    char tiny[3];
    TEST_ASSERT_TRUE(batfmt_percent(pct, sizeof(pct), &r));
    TEST_ASSERT_TRUE(batfmt_percent(pct, sizeof(pct), NULL));
    TEST_ASSERT_TRUE(batfmt_row(row, sizeof(row), &r));
    TEST_ASSERT_TRUE(batfmt_row(row, sizeof(row), &none));
    TEST_ASSERT_TRUE(batfmt_row(row, sizeof(row), NULL));
    TEST_ASSERT_FALSE(batfmt_percent(tiny, sizeof(tiny), &r));
    TEST_ASSERT_FALSE(batfmt_row(tiny, sizeof(tiny), &r));
}

void test_batfmt_run(void)
{
    RUN_TEST(test_batfmt_decodes_the_two_probed_states);
    RUN_TEST(test_batfmt_decodes_little_endian_from_the_raw_bytes);
    RUN_TEST(test_batfmt_maps_each_status_byte);
    RUN_TEST(test_batfmt_refuses_junk_and_leaves_out_alone);
    RUN_TEST(test_batfmt_check_does_not_overflow_in_32_bits);
    RUN_TEST(test_batfmt_cap_and_tolerance_edges);
    RUN_TEST(test_batfmt_fresh_until_exactly_fifteen_seconds);
    RUN_TEST(test_batfmt_level_is_monotonic_and_spans_the_range);
    RUN_TEST(test_batfmt_sends_the_first_frame);
    RUN_TEST(test_batfmt_sends_a_charge_change_alone);
    RUN_TEST(test_batfmt_ignores_a_one_point_move);
    RUN_TEST(test_batfmt_measures_a_two_point_move_from_the_last_sent);
    RUN_TEST(test_batfmt_sends_when_a_reading_comes_and_goes);
    RUN_TEST(test_batfmt_text_fits_the_buffers_and_reports_a_cut);
}
