#pragma once

#include "esp_err.h"
#include "lvgl.h"

/** Bring up the MIPI-DSI panel, LVGL, and the backlight. LDO 3 must already be on. */
esp_err_t display_init(lv_display_t **out_disp);
