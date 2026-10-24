#pragma once

/*
 * The sleep screen's configuration and layout math, pure so the host tests cover it. The screen is a 1024 x 512 image
 * area with the elements in a column at an anchor, and an 88 px strip below it for the spinner and the link status.
 */

#include <stdbool.h>
#include <stdint.h>

#define SLEEP_AREA_W 1024
#define SLEEP_AREA_H 512
#define SLEEP_STRIP_H 88
#define SLEEP_MARGIN 32 // from the area's edges to the element column
#define SLEEP_TEXT_MAX 65

typedef enum {
    SLEEP_ANCHOR_TOP_LEFT,
    SLEEP_ANCHOR_TOP_RIGHT,
    SLEEP_ANCHOR_BOTTOM_LEFT,
    SLEEP_ANCHOR_BOTTOM_RIGHT,
    SLEEP_ANCHOR_CENTER,
} sleep_anchor_t;

typedef enum {
    SLEEP_SPIN_ARC,
    SLEEP_SPIN_DOTS,
    SLEEP_SPIN_BAR,
    SLEEP_SPIN_PULSE,
    SLEEP_SPIN_NONE,
} sleep_spinner_t;

typedef struct {
    uint32_t bg, fg, spinner_color; // 0xRRGGBB
    bool show_clock, show_date, show_host, show_text;
    char text[SLEEP_TEXT_MAX];
    sleep_anchor_t anchor;
    sleep_spinner_t spinner;
    uint16_t offline_after_s; // 0 never sleeps for a lost link
    uint8_t backlight;        // percent while asleep
} sleep_cfg_t;

/** The built-in screen: black, light grey text, the clock and date at the bottom left, an arc spinner. */
void sleepcfg_defaults(sleep_cfg_t *cfg);

/** The elements, top to bottom. */
typedef enum {
    SLEEP_EL_CLOCK,
    SLEEP_EL_DATE,
    SLEEP_EL_HOST,
    SLEEP_EL_TEXT,
    SLEEP_EL_COUNT,
} sleep_el_t;

/** What data exists right now. The clock and the date need the time from a `stats` frame, the host a `welcome`. */
typedef struct {
    bool time, host;
} sleep_have_t;

typedef struct {
    bool shown;
    int x, y, w, h; // inside the image area
} sleep_el_pos_t;

/**
 * Where each element goes. `heights` are the fonts' line heights. The column is SLEEP_MARGIN from the area's edges
 * (centered vertically for the center anchor), every element is as wide as the column, and the elements stack with no
 * gap. An element whose data is missing, or that `cfg` hides, is not shown and takes no room.
 */
void sleepcfg_layout(const sleep_cfg_t *cfg, sleep_have_t have, const int heights[SLEEP_EL_COUNT],
                     sleep_el_pos_t out[SLEEP_EL_COUNT]);

/** How the text sits in its element: -1 left, 0 centered, 1 right. */
int sleepcfg_text_align(sleep_anchor_t anchor);

/**
 * Whether the link has been down long enough to sleep: `offline_after_s` seconds both of link loss (`down_ms`) and of
 * no touch (`idle_ms`). 0 never.
 */
bool sleepcfg_offline_due(uint16_t offline_after_s, uint32_t down_ms, uint32_t idle_ms);
