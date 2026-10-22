#pragma once

#include <stdbool.h>

#include "font_list.h"

/* Font ids as the wire and config.toml name them. No LVGL here, so the host tests can use it. */

typedef enum {
#define X(id, name) WD_FONT_##id,
    WD_FONT_LIST(X)
#undef X
        WD_FONT_COUNT,
} wd_font_t;

#define WD_FONT_MONTSERRAT WD_FONT_montserrat

/**
 * Look up a font id by its wire name. An unknown name (or NULL) gives Montserrat, and clears `*known`
 * when `known` is not NULL.
 */
wd_font_t wd_font_from_name(const char *name, bool *known);
