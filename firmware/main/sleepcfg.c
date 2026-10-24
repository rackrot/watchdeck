#include "sleepcfg.h"

#include <string.h>

// A temporary build constant to look at each spinner on the board (-DWD_SLEEP_SPINNER=SLEEP_SPIN_DOTS).
#ifndef WD_SLEEP_SPINNER
#define WD_SLEEP_SPINNER SLEEP_SPIN_ARC
#endif

void sleepcfg_defaults(sleep_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->bg = 0x000000;
    cfg->fg = 0xd0d0d0;
    cfg->spinner_color = 0x38bdf8; // the app's accent
    cfg->show_clock = true;
    cfg->show_date = true;
    cfg->anchor = SLEEP_ANCHOR_BOTTOM_LEFT;
    cfg->spinner = WD_SLEEP_SPINNER;
    cfg->offline_after_s = 10;
    cfg->backlight = 100;
}

void sleepcfg_layout(const sleep_cfg_t *cfg, sleep_have_t have, const int heights[SLEEP_EL_COUNT],
                     sleep_el_pos_t out[SLEEP_EL_COUNT])
{
    const bool shown[SLEEP_EL_COUNT] = {
        [SLEEP_EL_CLOCK] = cfg->show_clock && have.time,
        [SLEEP_EL_DATE] = cfg->show_date && have.time,
        [SLEEP_EL_HOST] = cfg->show_host && have.host,
        [SLEEP_EL_TEXT] = cfg->show_text && cfg->text[0] != '\0',
    };
    int column_h = 0;
    for (int i = 0; i < SLEEP_EL_COUNT; i++) {
        column_h += shown[i] ? heights[i] : 0;
    }
    int y = SLEEP_MARGIN;
    if (cfg->anchor == SLEEP_ANCHOR_BOTTOM_LEFT || cfg->anchor == SLEEP_ANCHOR_BOTTOM_RIGHT) {
        y = SLEEP_AREA_H - SLEEP_MARGIN - column_h;
    } else if (cfg->anchor == SLEEP_ANCHOR_CENTER) {
        y = (SLEEP_AREA_H - column_h) / 2;
    }
    for (int i = 0; i < SLEEP_EL_COUNT; i++) {
        out[i] = (sleep_el_pos_t){
            .shown = shown[i], .x = SLEEP_MARGIN, .y = y, .w = SLEEP_AREA_W - 2 * SLEEP_MARGIN, .h = heights[i]};
        if (shown[i]) {
            y += heights[i];
        }
    }
}

int sleepcfg_text_align(sleep_anchor_t anchor)
{
    switch (anchor) {
    case SLEEP_ANCHOR_TOP_RIGHT:
    case SLEEP_ANCHOR_BOTTOM_RIGHT:
        return 1;
    case SLEEP_ANCHOR_CENTER:
        return 0;
    default:
        return -1;
    }
}

bool sleepcfg_offline_due(uint16_t offline_after_s, uint32_t down_ms, uint32_t idle_ms)
{
    const uint64_t after_ms = (uint64_t)offline_after_s * 1000;
    return offline_after_s > 0 && down_ms >= after_ms && idle_ms >= after_ms;
}
