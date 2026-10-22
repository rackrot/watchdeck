#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pageset.h"
#include "proto.h"

/*
 * What the board shows, shared by the link task and the LVGL task: the pages of the applied set, the latest value of
 * each tile button, the page on screen, and (through icons.c) the icon cache. All of it is in PSRAM behind one mutex.
 *
 * Lock order: the LVGL lock first, then the model lock, never the other way round. A path that needs both takes
 * lvgl_port_lock first. The link task never calls a UI callback while it holds the model lock.
 */

#define MODEL_MAX_TILES PROTO_MAX_BUTTONS // a config has at most 40 tile buttons across its pages

/** Allocate the model and the icon table in PSRAM, and create the lock. Call once, before link_start. */
esp_err_t model_init(void);

void model_lock(void);
void model_unlock(void);

/* Everything below needs the model lock. */

/**
 * Make the set `ps` just completed the applied set, by swapping storage: `ps->pages` gets the old set's slots back for
 * staging the next one. Clears every stored tile value, since the daemon resends each tile after a set.
 */
void model_apply(pageset_t *ps);

/** Pages in the applied set: 0 before the first set, else 1 to PROTO_MAX_PAGES. */
uint8_t model_pages(void);

/** The rev of the applied set. */
uint32_t model_rev(void);

/** Page `i` of the applied set, below model_pages(). Valid until the lock is released. */
const proto_layout_t *model_page(uint8_t i);

/**
 * The index of the page on screen: the page whose id was shown last, or the home page when the set has no such page,
 * which then becomes the page shown. Call only with model_pages() > 0.
 */
uint8_t model_shown(void);

/**
 * Store a tile's value if a page of the applied set has a tile button with its id. Returns false, and logs why, when
 * there is none or the store is full.
 */
bool model_set_tile(const proto_tile_t *tile);

/** The stored value of tile button `btn`, or NULL before its first value. Valid until the lock is released. */
const proto_tile_t *model_tile(const char *btn);

/** Forget every stored tile value. */
void model_clear_tiles(void);

/** Forget the applied set, the page shown, the tile values and every cached icon. Needs the LVGL lock first too. */
void model_clear(void);
