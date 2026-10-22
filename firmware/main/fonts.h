#pragma once

#include "font_ids.h"
#include "grid.h" // SLOT24_H and SLOT32_H, the slots' line boxes
#include "lvgl.h"

/* The grid has two text slots, named by their Montserrat size. Their line boxes are fixed, whatever the font. */
typedef enum {
    WD_SLOT_24,
    WD_SLOT_32,
} wd_slot_t;

/** The font for `id` at `slot`. An out-of-range id gives Montserrat. */
const lv_font_t *wd_font(wd_font_t id, wd_slot_t slot);
