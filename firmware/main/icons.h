#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"
#include "proto.h"

/* The icons of the current layout, in PSRAM, by id. Everything here runs on the link task only. The UI gets the
   image descriptors through the link's on_icon callback and holds them in image widgets. */

#define ICONS_MAX PROTO_MAX_BUTTONS // one per cell, since a layout never names more
#define ICONS_MAX_BYTES (8 * 1024 * 1024)

/** The cached image for `id`, or NULL. It stays valid until icons_retain or icons_clear drops it. */
const lv_image_dsc_t *icons_find(const uint8_t id[PROTO_ICON_ID_LEN]);

/**
 * Cache a frame that proto_parse_icon accepted. `frame` is the PSRAM allocation `icon->pixels` points into. On
 * ESP_OK the cache owns it. On any other code (already cached, full, over the byte cap) the caller still does.
 */
esp_err_t icons_add(const proto_icon_t *icon, uint8_t *frame);

/** Free every cached icon `layout` does not name, and return how many that was. Call after the UI dropped the old grid.
 */
size_t icons_retain(const proto_layout_t *layout);

/** Free every cached icon. Call after the UI dropped the grid. */
void icons_clear(void);
