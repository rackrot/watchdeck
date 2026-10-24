#pragma once

/*
 * The screen modes and what each UI entry point does in each of them. Pure, no LVGL, so the host tests check every cell
 * of the table: a new mode that forgets an entry point fails there instead of building widgets under a full-screen
 * mode.
 *
 * Entry points that act the same in every mode are not in the table: ui_set_stats keeps the frame (the bar and the
 * sleep screen's clock read it), ui_link_down greys the grid and updates the bar or the sleep strip,
 * ui_set_paired(false) ends whatever mode is up and shows status mode.
 */

#include <stdbool.h>

/**
 * Status mode runs from boot until the first set applies and again after every unpair. Grid mode runs from the first
 * set until an unpair. Settings and the sleep screen each replace the grid (or status mode's lines) while up and go
 * back to the mode they came from.
 */
typedef enum {
    MODE_STATUS,
    MODE_GRID,
    MODE_SETTINGS,
    MODE_SLEEP,
    MODE_COUNT,
} ui_mode_t;

typedef enum {
    UIM_SHOW_SET,    // ui_show_set: the link applied a set
    UIM_SET_TILE,    // ui_set_tile
    UIM_SET_ICON,    // ui_set_icon
    UIM_SHOW_RESULT, // ui_show_result: the toast and the result modal
    UIM_DAEMON_LIST, // ui_show_daemons: the list of desktops to pair with
    UIM_PAIR_CODE,   // ui_show_pair_code and ui_show_pair_confirm
    UIM_FEED,        // ui_set_feed: a snapshot is in the model; the -1 page, if up, shows it
    UIM_ENTRY_COUNT,
} ui_entry_t;

typedef enum {
    UI_ACTION_NONE, // never in the table: a cell left at 0 is a mode that forgot the entry point
    UI_BUILD,       // act now, building widgets as before
    UI_DEFER,       // keep the data, build nothing; the mode rebuilds when it ends
    UI_DROP,        // keep only bookkeeping and build nothing; there is nothing to rebuild
} ui_action_t;

/** Why the sleep screen is up. */
typedef enum {
    SLEEP_OFFLINE, // the link was down for offline_after_s
    SLEEP_PRIVACY, // a long swipe down
} sleep_reason_t;

/** What `entry` does while the UI is in `mode`. */
ui_action_t uimode_action(ui_entry_t entry, ui_mode_t mode);

/**
 * Whether the sleep screen may start now. Never unpaired and never from settings or while already asleep. A privacy
 * sleep (a swipe on the grid) starts from the grid only; an offline sleep from the grid or status mode.
 */
bool uimode_can_sleep(ui_mode_t mode, sleep_reason_t reason, bool paired);

/** Whether a set applying (the link coming back) ends a sleep of this reason. Privacy stays until a touch. */
bool uimode_link_wakes(sleep_reason_t reason);
