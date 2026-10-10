#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include "proto.h"

/** Build the status lines and the toast on the active screen of `disp`. The grid comes from the daemon. */
esp_err_t ui_init(lv_display_t *disp);

/** Thread-safe status line updates. Ignored before ui_init succeeds. */
void ui_set_wifi_text(const char *text);
void ui_set_sd_text(const char *text);
void ui_set_usb_text(const char *text);
void ui_set_touch_text(const char *text);
void ui_set_daemon_text(const char *text);

/** Replace the grid with `layout` and accept presses. Thread-safe. */
void ui_apply_layout(const proto_layout_t *layout);

/** Grey out the grid and block presses until the next ui_apply_layout. Thread-safe. */
void ui_link_down(void);

/** Show a toast for the result of a press this UI sent. Results with an unknown seq are ignored. */
void ui_show_result(const proto_result_t *result);
