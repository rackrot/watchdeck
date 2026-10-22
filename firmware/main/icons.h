#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "proto.h"

/* The icons of every page of the applied set, in PSRAM, by id. Part of the model (model.h): every call here needs the
   model lock, and icons_retain and icons_clear also need the LVGL lock taken before it, since dropping an icon takes
   the LVGL lock. The UI reads the image descriptors under the model lock and holds them in image widgets. */

#define ICONS_MAX (PROTO_MAX_PAGES * PROTO_MAX_BUTTONS) // one per cell of every page, since a set never names more
#define ICONS_MAX_BYTES (8 * 1024 * 1024) // a valid set needs at most 4,844,160 bytes (test_grid.c pins it)

/** Allocate the table in PSRAM. model_init calls it once. */
esp_err_t icons_init(void);

/** The cached image for `id`, or NULL. It stays valid until icons_retain or icons_clear drops it. */
const lv_image_dsc_t *icons_find(const uint8_t id[PROTO_ICON_ID_LEN]);

/**
 * Cache a frame that proto_parse_icon accepted. `frame` is the PSRAM allocation `icon->pixels` points into. On
 * ESP_OK the cache owns it. On any other code (already cached, full, over the byte cap) the caller still does.
 */
esp_err_t icons_add(const proto_icon_t *icon, uint8_t *frame);

/**
 * Free every cached icon that none of the `n` pages names, and return how many that was. Call after the UI replaced
 * the widgets that showed them.
 */
size_t icons_retain(const proto_layout_t *pages, size_t n);

/** Free every cached icon. Call after the UI dropped the grid. */
void icons_clear(void);
