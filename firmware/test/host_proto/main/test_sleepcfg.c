// sleepcfg.c: the sleep screen's defaults, its element layout in the 1024 x 512 image area, and the offline rule.

#include <string.h>

#include "unity.h"

#include "sleepcfg.h"

static const int HEIGHTS[SLEEP_EL_COUNT] = {55, 37, 37, 37}; // Montserrat 48 and 32 line heights, or near enough
#define ALL ((sleep_have_t){.time = true, .host = true})

static void all_shown_cfg(sleep_cfg_t *cfg)
{
    sleepcfg_defaults(cfg);
    cfg->show_host = true;
    cfg->show_text = true;
    strcpy(cfg->text, "away");
}

void test_sleepcfg_defaults(void)
{
    sleep_cfg_t cfg;
    sleepcfg_defaults(&cfg);
    TEST_ASSERT_EQUAL_HEX32(0x000000, cfg.bg);
    TEST_ASSERT_EQUAL_HEX32(0x38bdf8, cfg.spinner_color);
    TEST_ASSERT_TRUE(cfg.show_clock && cfg.show_date);
    TEST_ASSERT_FALSE(cfg.show_host || cfg.show_text);
    TEST_ASSERT_EQUAL(SLEEP_ANCHOR_BOTTOM_LEFT, cfg.anchor);
    TEST_ASSERT_EQUAL(SLEEP_SPIN_ARC, cfg.spinner);
    TEST_ASSERT_EQUAL_UINT16(10, cfg.offline_after_s);
    TEST_ASSERT_EQUAL_UINT8(100, cfg.backlight);
}

// Each anchor: a column 32 px from the area's edges, the clock on top, no gaps, nothing outside the area.
void test_sleepcfg_layout_every_anchor_stays_inside_the_margins(void)
{
    const int col_h = 55 + 37 + 37 + 37;
    const struct {
        sleep_anchor_t anchor;
        int first_y;
    } cases[] = {
        {SLEEP_ANCHOR_TOP_LEFT, SLEEP_MARGIN},
        {SLEEP_ANCHOR_TOP_RIGHT, SLEEP_MARGIN},
        {SLEEP_ANCHOR_BOTTOM_LEFT, SLEEP_AREA_H - SLEEP_MARGIN - col_h},
        {SLEEP_ANCHOR_BOTTOM_RIGHT, SLEEP_AREA_H - SLEEP_MARGIN - col_h},
        {SLEEP_ANCHOR_CENTER, (SLEEP_AREA_H - col_h) / 2},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        sleep_cfg_t cfg;
        all_shown_cfg(&cfg);
        cfg.anchor = cases[c].anchor;
        sleep_el_pos_t pos[SLEEP_EL_COUNT];
        sleepcfg_layout(&cfg, ALL, HEIGHTS, pos);
        int y = cases[c].first_y;
        for (int i = 0; i < SLEEP_EL_COUNT; i++) {
            char msg[40];
            snprintf(msg, sizeof(msg), "anchor %d element %d", (int)cases[c].anchor, i);
            TEST_ASSERT_TRUE_MESSAGE(pos[i].shown, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(SLEEP_MARGIN, pos[i].x, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(SLEEP_AREA_W - 2 * SLEEP_MARGIN, pos[i].w, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(y, pos[i].y, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(HEIGHTS[i], pos[i].h, msg);
            y += HEIGHTS[i];
        }
        TEST_ASSERT_GREATER_OR_EQUAL_INT(SLEEP_MARGIN, pos[0].y);
        TEST_ASSERT_LESS_OR_EQUAL_INT(SLEEP_AREA_H - SLEEP_MARGIN, y);
    }
}

void test_sleepcfg_layout_hides_what_has_no_data(void)
{
    sleep_cfg_t cfg;
    all_shown_cfg(&cfg);
    sleep_el_pos_t pos[SLEEP_EL_COUNT];

    // A reboot with no daemon: no time and no host. Only the text is left, alone at the bottom of the column.
    sleepcfg_layout(&cfg, (sleep_have_t){0}, HEIGHTS, pos);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_CLOCK].shown);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_DATE].shown);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_HOST].shown);
    TEST_ASSERT_TRUE(pos[SLEEP_EL_TEXT].shown);
    TEST_ASSERT_EQUAL_INT(SLEEP_AREA_H - SLEEP_MARGIN - 37, pos[SLEEP_EL_TEXT].y);

    // The time is known, the host is not.
    sleepcfg_layout(&cfg, (sleep_have_t){.time = true}, HEIGHTS, pos);
    TEST_ASSERT_TRUE(pos[SLEEP_EL_CLOCK].shown && pos[SLEEP_EL_DATE].shown);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_HOST].shown);

    // An empty text is hidden even when asked for, and a hidden element takes no room.
    cfg.text[0] = '\0';
    cfg.show_clock = false;
    sleepcfg_layout(&cfg, ALL, HEIGHTS, pos);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_CLOCK].shown);
    TEST_ASSERT_FALSE(pos[SLEEP_EL_TEXT].shown);
    TEST_ASSERT_EQUAL_INT(SLEEP_AREA_H - SLEEP_MARGIN - 2 * 37, pos[SLEEP_EL_DATE].y);
    TEST_ASSERT_EQUAL_INT(pos[SLEEP_EL_DATE].y + 37, pos[SLEEP_EL_HOST].y);
}

void test_sleepcfg_text_align_follows_the_anchor(void)
{
    TEST_ASSERT_EQUAL_INT(-1, sleepcfg_text_align(SLEEP_ANCHOR_TOP_LEFT));
    TEST_ASSERT_EQUAL_INT(-1, sleepcfg_text_align(SLEEP_ANCHOR_BOTTOM_LEFT));
    TEST_ASSERT_EQUAL_INT(1, sleepcfg_text_align(SLEEP_ANCHOR_TOP_RIGHT));
    TEST_ASSERT_EQUAL_INT(1, sleepcfg_text_align(SLEEP_ANCHOR_BOTTOM_RIGHT));
    TEST_ASSERT_EQUAL_INT(0, sleepcfg_text_align(SLEEP_ANCHOR_CENTER));
}

void test_sleepcfg_offline_due_needs_link_loss_and_no_touch_for_the_delay(void)
{
    TEST_ASSERT_FALSE(sleepcfg_offline_due(10, 9999, 20000)); // the link has not been down long enough
    TEST_ASSERT_FALSE(sleepcfg_offline_due(10, 20000, 9999)); // a touch was recent
    TEST_ASSERT_TRUE(sleepcfg_offline_due(10, 10000, 10000)); // both at the delay
    TEST_ASSERT_TRUE(sleepcfg_offline_due(1, 60000, 60000));
    TEST_ASSERT_TRUE(sleepcfg_offline_due(65535, UINT32_MAX, UINT32_MAX));
    TEST_ASSERT_FALSE(sleepcfg_offline_due(65535, 65534999, 65534999));
}

void test_sleepcfg_offline_after_0_never_triggers(void)
{
    TEST_ASSERT_FALSE(sleepcfg_offline_due(0, 0, 0));
    TEST_ASSERT_FALSE(sleepcfg_offline_due(0, UINT32_MAX, UINT32_MAX));
}

void test_sleepcfg_run(void)
{
    RUN_TEST(test_sleepcfg_defaults);
    RUN_TEST(test_sleepcfg_layout_every_anchor_stays_inside_the_margins);
    RUN_TEST(test_sleepcfg_layout_hides_what_has_no_data);
    RUN_TEST(test_sleepcfg_text_align_follows_the_anchor);
    RUN_TEST(test_sleepcfg_offline_due_needs_link_loss_and_no_touch_for_the_delay);
    RUN_TEST(test_sleepcfg_offline_after_0_never_triggers);
}
