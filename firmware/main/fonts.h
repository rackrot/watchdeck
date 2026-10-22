#pragma once

#include "font_ids.h"
#include "lvgl.h"

/* The grid has two text slots, named by their Montserrat size. Their line boxes are fixed, whatever the font. */
typedef enum {
    WD_SLOT_24,
    WD_SLOT_32,
} wd_slot_t;

#define SLOT24_H 27 // Montserrat 24's line height
#define SLOT32_H 35 // Montserrat 32's line height

/** The font for `id` at `slot`. An out-of-range id gives Montserrat. */
const lv_font_t *wd_font(wd_font_t id, wd_slot_t slot);
