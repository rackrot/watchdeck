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

/** Replace the grid with `layout` and accept presses. Every layout the daemon pushes lands here. Thread-safe. */
void ui_apply_layout(const proto_layout_t *layout);

/**
 * Show `dsc` above the label of every button whose layout icon id is `id`. `dsc` must outlive those buttons:
 * the link drops an icon only after ui_apply_layout has replaced the grid that showed it. Thread-safe.
 */
void ui_set_icon(const uint8_t id[PROTO_ICON_ID_LEN], const lv_image_dsc_t *dsc);

/** Grey out the grid and block presses until the next ui_apply_layout. Thread-safe. */
void ui_link_down(void);

/**
 * Show the result of a press this UI sent: OK or Failed, the summary and the output, until a tap or a few
 * seconds pass. Results with an unknown seq are ignored. Thread-safe.
 */
void ui_show_result(const proto_result_t *result);

/**
 * Show a tile's value on its button, in place, without rebuilding the grid: the text on one line and the button
 * red for a bad state. Ignored for a button the grid doesn't have or that has no tile. Thread-safe.
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
