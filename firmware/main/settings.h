#pragma once

/*
 * The settings page: a tab row under the bar (or under the "Settings" band in status mode) with Close at its right end,
 * and one tab's content below. Each tab is built when shown and deleted when left, so two tabs are never in the LVGL
 * pool together. ui.c owns the mode switch around it; the tabs are in settings.c's table.
 */

#include "grid.h"
#include "logring.h"
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

/**
 * Let the shown tab turn off the page's swipe-down close while a view that needs drags is up (the Wi-Fi editor's
 * keyboard and text areas). Back on at every tab switch and page open, so a tab turns it on and off itself.
 */
void settings_set_swipe_close(bool on);

/** The first set applied under settings opened from status mode: the bar is back, so drop the "Settings" band title. */
void settings_bar_returned(void);

/*
 * Log lines for the Logs and Errors tabs. A line is a clickable one-line label in Montserrat 20, clipped, with recolor
 * off (log text can hold '#'). Its text is never copied into the LVGL pool: the label points at the caller's copy,
 * which must outlive the label or be replaced by settings_log_line_set first.
 */
#define LOG_LINE_H 28     // the Logs tab's line height, about 6 mm to tap
#define LOG_LINE_MIN_H 24 // as tight as Montserrat 20's 23 px line allows, for the Errors tab

/** Create a line label at (x, y) of size w by h. A tap calls `on_tap` with `user_data`, set as the event's user data.
 */
lv_obj_t *settings_log_line_create(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t on_tap, void *user_data);

/** Show `line` (NULL for none) on a line label, in the colour of its level (E red, W yellow). `line` must stay valid.
 */
void settings_log_line_set(lv_obj_t *label, const logring_line_t *line);

/** Open `line` whole, wrapped, with its level and time since boot, in a modal on the top layer. A tap closes it. */
void settings_log_line_open(const logring_line_t *line);

/** Close that modal, at once. A tab that can open it calls this from its `leave`. */
void settings_log_line_close(void);

/* The tabs, for settings.c's table. */
void tab_status_build(lv_obj_t *parent);
void tab_status_tick(void);
void tab_status_leave(void);
void tab_wifi_build(lv_obj_t *parent);
void tab_wifi_tick(void);
void tab_wifi_leave(void);
/** Whether the Wi-Fi editor (the typed network and password, or its keyboard) is up, so the idle close waits. */
bool tab_wifi_editing(void);
void tab_logs_build(lv_obj_t *parent);
void tab_logs_tick(void);
void tab_logs_leave(void);
void tab_errors_build(lv_obj_t *parent);
void tab_errors_tick(void);
void tab_errors_leave(void);
