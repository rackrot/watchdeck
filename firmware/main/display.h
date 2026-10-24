#pragma once

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_H_RES 1024
#define DISPLAY_V_RES 600

/** Bring up the MIPI-DSI panel, LVGL, and the backlight. LDO 3 must already be on. */
esp_err_t display_init(lv_display_t **out_disp);

/**
 * The backlight level in percent. Until P0 shows that GPIO31 dims cleanly from PWM, it is on or off: 0 turns the light
 * off and anything else turns it on. Touch still works with the light off.
 */
void display_backlight(uint8_t percent);
