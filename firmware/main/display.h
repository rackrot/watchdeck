#pragma once

#include "esp_err.h"

/** Bring up the MIPI-DSI panel, LVGL, and the hello screen. LDO 3 must already be on. */
esp_err_t display_init(void);

/** Thread-safe text updates for the hello screen. Ignored before display_init succeeds. */
void display_set_wifi_text(const char *text);
void display_set_sd_text(const char *text);
void display_set_usb_text(const char *text);
