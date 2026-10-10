#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * Bring up the GT911 on I2C port 0 and register it with LVGL as a pointer for `disp`.
 * LDO 4 (the I2C pull-up rail) must already be on. Polled, since INT has no pull-up.
 */
esp_err_t touch_init(lv_display_t *disp);
