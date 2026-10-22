#pragma once

#include "esp_err.h"
#include "link.h"
#include "lvgl.h"
#include "proto.h"

/** Build the status lines, the toast and the result modal on `disp`. The grid comes from the daemon. */
esp_err_t ui_init(lv_display_t *disp);

/** Thread-safe status line updates. Ignored before ui_init succeeds. */
void ui_set_wifi_text(const char *text);
void ui_set_sd_text(const char *text);
void ui_set_usb_text(const char *text);
void ui_set_touch_text(const char *text);
void ui_set_daemon_text(const char *text);

/**
 * The link applied a set of pages to the model: rebuild the page on screen from it (the home page if the set lacks
 * it), and accept presses. Takes the LVGL lock, then the model lock. Thread-safe, but not under the model lock.
 */
void ui_show_set(void);

/**
 * Show `dsc` above the label of every button on screen whose icon id is `id`. `dsc` must outlive those buttons:
 * the link drops an icon only after ui_show_set has replaced the grid that showed it. Thread-safe.
 */
void ui_set_icon(const uint8_t id[PROTO_ICON_ID_LEN], const lv_image_dsc_t *dsc);

/** Every icon of the set has come, or stopped coming: log a settled memory sample. Thread-safe. */
void ui_icons_settled(void);

/** Grey out the grid and block presses until the next ui_show_set. Thread-safe. */
void ui_link_down(void);

/**
 * Show the result of a press this UI sent: OK or Failed, the summary and the output, until a tap or a few
 * seconds pass. Results with an unknown seq are ignored. Thread-safe.
 */
void ui_show_result(const proto_result_t *result);

/**
 * Show a tile's value on its button, in place, without rebuilding the grid: the text on one line and the button
 * red for a bad state. The model already holds the value, so a button on another page shows it when its page is
 * built. Thread-safe.
 */
void ui_set_tile(const proto_tile_t *tile);

/* The pairing views below are thread-safe and ignored before ui_init succeeds. */

/** Paired: show the grid area and Forget pairing. Unpaired: show the pairable daemons. */
void ui_set_paired(bool paired);

/** Replace the list of daemons open for pairing (unpaired only). */
void ui_show_daemons(const link_daemon_t *daemons, size_t n);

/** Show the pairing code for `name`, or with `code` NULL go back to the list. */
void ui_show_pair_code(const char *code, const char *name);

/** Ask whether the watchdeck app on `name` shows "Paired <device>", with Yes and No. */
void ui_show_pair_confirm(const char *device, const char *name);
