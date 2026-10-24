#pragma once

/*
 * The settings page: a tab row under the bar (or under the "Settings" band in status mode) with Close at its right end,
 * and one tab's content below. Each tab is built when shown and deleted when left, so two tabs are never in the LVGL
 * pool together. ui.c owns the mode switch around it; the tabs are in settings.c's table.
 */

#include "grid.h"
#include "lvgl.h"
#include "ui_priv.h"

#define SETTINGS_PAD 16
#define SETTINGS_TAB_ROW_H 64
// A tab's content fills the page below the tab row, and a tab's widgets are positioned inside it.
#define SETTINGS_CONTENT_Y (GRID_TOP + SETTINGS_TAB_ROW_H)
#define SETTINGS_CONTENT_W GRID_SCREEN_W
#define SETTINGS_CONTENT_H (GRID_SCREEN_H - SETTINGS_CONTENT_Y)

/*
 * A tab. All run in the LVGL task, under the LVGL lock.
 *  build: create the tab's widgets under `parent`, the content object, which settings.c deletes after leave.
 *  tick:  once a second while the tab is shown, to refresh it. May be NULL.
 *  leave: just before the content is deleted, to forget pointers into it and stop anything the tab started. May be
 * NULL.
 */
typedef struct {
    const char *name;
    void (*build)(lv_obj_t *parent);
    void (*tick)(void);
    void (*leave)(void);
} settings_tab_t;

/**
 * Build the page on the active screen, opened from mode `from` (MODE_GRID or MODE_STATUS), on the last tab shown. The
 * caller has deleted the grid or status widgets already, and calls log_heap after.
 */
void settings_build(ui_mode_t from);

/** Delete the page, its timer and the shown tab. Does nothing when closed. */
void settings_delete(void);

bool settings_is_open(void);

/** The first set applied under settings opened from status mode: the bar is back, so drop the "Settings" band title. */
void settings_bar_returned(void);

/* The tabs, for settings.c's table. */
void tab_status_build(lv_obj_t *parent);
void tab_status_tick(void);
void tab_status_leave(void);
