#pragma once

/*
 * What ui.c shares with the screens built on it: the settings page and its tabs today, the sleep screen (P1) and the
 * feed page (N3) next. Nothing outside the UI includes this. Every function runs in the LVGL task or under the LVGL
 * lock, except where noted.
 */

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

/*
 * The screen mode. Status mode runs from boot until the first set applies, and again after every unpair. Grid mode
 * runs from the first set until an unpair. Settings replaces the grid (or status mode's lines) while open and goes
 * back to the mode it came from.
 *
 * Hooks: P1 adds MODE_SLEEP here and in ui.c's mode switch. N3 adds no mode: the feed page is a sub-state of
 * MODE_GRID (an `on_feed` flag in ui.c).
 */
typedef enum {
    MODE_STATUS,
    MODE_GRID,
    MODE_SETTINGS,
} ui_mode_t;

/** The five status lines, kept for status mode and for settings. */
typedef enum {
    STATUS_WIFI,
    STATUS_SD,
    STATUS_USB,
    STATUS_TOUCH,
    STATUS_DAEMON,
    STATUS_COUNT,
} status_t;

#define STATUS_TEXT_MAX 96 // the longest status text link.c, wifi.c and usb.c build, and its NUL

ui_mode_t ui_mode(void);

/** Whether the board holds a pairing. */
bool ui_paired(void);

/** The kept text of status line `i`. The daemon's has no "Daemon: " prefix and only printable ASCII. */
const char *ui_status_text(status_t i);

/** The daemon's host name from the last welcome, "" until then and after an unpair. */
const char *ui_daemon_name(void);

/** Put `text` on `label` unless `cache` already holds it. A LONG_DOT label edits its own text, so the cache is the
 * source. */
void ui_set_label(lv_obj_t *label, char *cache, size_t cache_size, const char *text);

/** One log line to compare memory at the same point across runs: the LVGL pool, then the internal and PSRAM heaps. */
void ui_log_heap(const char *when);

/** The Forget pairing button (hold 2 s) and the Licenses button, to place in any screen that offers them. */
lv_obj_t *ui_add_forget(lv_obj_t *parent);
lv_obj_t *ui_add_licenses_button(lv_obj_t *parent);

/** True while the font license viewer is up. */
bool ui_licenses_open(void);

/** Close settings and rebuild the mode it came from. Asks through lv_async_call, so it is safe in any widget's event.
 */
void ui_settings_close(void);
