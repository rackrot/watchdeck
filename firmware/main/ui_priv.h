#pragma once

/*
 * What ui.c shares with the screens built on it: the settings page and its tabs today, the sleep screen (P1) and the
 * feed page (N3) next. Nothing outside the UI includes this. Every function runs in the LVGL task or under the LVGL
 * lock, except where noted.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/*
 * The screen modes are in uimode.h, with the table of what each entry point does in each. The feed page adds no mode:
 * it is a sub-state of MODE_GRID (s_on_feed in ui.c), shown in the grid's place by build_page.
 */
#include "uimode.h"

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

/** Whether the link is up (a session is open and a set has applied since it opened). */
bool ui_online(void);

/**
 * The host's local time now as `clock` "14:32" and `date` "Wed 22 Oct", from the last `stats` frame. False when no
 * frame has come since this boot or the unpair: the time is then unknown.
 */
bool ui_local_clock(char *clock, size_t clock_size, char *date, size_t date_size);

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

/**
 * For the feed page (feedpage.c), whose note rows track touches and take taps like the grid's cells: the touch tracking
 * that opens settings on a long swipe up, and the check a tap runs first, true when the touch was a drag (and then it
 * may have turned the page).
 */
void ui_touch_track(lv_event_t *e);
bool ui_drag_consumed(const char *what);

/** The host's clock in Unix seconds, run on from the latest `stats` frame. Returns false, and leaves `now`, before the
 * first. */
bool ui_clock_now(uint64_t *now);

/** Close settings and rebuild the mode it came from. Asks through lv_async_call, so it is safe in any widget's event.
 */
void ui_settings_close(void);
