#pragma once

/*
 * The sleep screen's widgets: a full-screen object with the image area (the logo placeholder until L4 and P3 bring
 * art), the elements of sleepcfg.h, and the strip with the spinner and the link status. ui.c owns the mode, the reason
 * and the wake; this file only draws. Everything runs in the LVGL task or under the LVGL lock.
 */

#include <stdbool.h>

#include "lvgl.h"
#include "sleepcfg.h"

/** Load the built-in configuration. Call once, from ui_init. */
void sleep_init(void);

/** The configuration in use. P3 replaces it from the daemon's `sleep` frame or the card. */
const sleep_cfg_t *sleep_config(void);

/**
 * Build the screen on the active screen and set the backlight. `on_touch` is called from the screen's event when a
 * touch that began on it is released; it must not delete the screen there (use lv_async_call). Does nothing when the
 * screen is already up.
 */
void sleep_show(void (*on_touch)(void));

/**
 * Use `cfg` from now on (copied). If the screen is up it is rebuilt in place from it; the backlight and the touch
 * callback are not touched, so the screen does not flash or lose its wake handler. Takes no lock itself: the caller
 * must hold the LVGL lock (lvgl_port_lock) or be in the LVGL task, like every function here. P3 calls it from the link
 * and boot jobs, which take the lock first.
 */
void sleep_set_config(const sleep_cfg_t *cfg);

/** Delete the screen and restore the backlight to full. Does nothing when it is not up. */
void sleep_hide(void);

bool sleep_is_shown(void);

/** Bring the clock, host, strip and spinner up to date with the UI's state. Also runs once a second while shown. */
void sleep_refresh(void);
