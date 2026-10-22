#include "fonts.h"

// Montserrat is LVGL's built-in; the others are generated (fonts/generate.mjs). Pasting
// wd_font_<id>_<slot> then names each of them, with Montserrat mapped by these two aliases.
#define wd_font_montserrat_24 lv_font_montserrat_24
#define wd_font_montserrat_32 lv_font_montserrat_32

#define X(id, name) LV_FONT_DECLARE(wd_font_##id##_24) LV_FONT_DECLARE(wd_font_##id##_32)
WD_FONT_LIST(X)
#undef X

static const lv_font_t *const TABLE[WD_FONT_COUNT][2] = {
#define X(id, name) [WD_FONT_##id] = {[WD_SLOT_24] = &wd_font_##id##_24, [WD_SLOT_32] = &wd_font_##id##_32},
    WD_FONT_LIST(X)
#undef X
};

const lv_font_t *wd_font(wd_font_t id, wd_slot_t slot)
{
    return TABLE[id < WD_FONT_COUNT ? id : WD_FONT_MONTSERRAT][slot];
}
