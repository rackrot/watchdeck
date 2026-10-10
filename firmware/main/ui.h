#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** Build the status lines and the touch test grid on the active screen of `disp`. */
esp_err_t ui_init(lv_display_t *disp);

/** Thread-safe status line updates. Ignored before ui_init succeeds. */
void ui_set_wifi_text(const char *text);
void ui_set_sd_text(const char *text);
void ui_set_usb_text(const char *text);
void ui_set_touch_text(const char *text);
