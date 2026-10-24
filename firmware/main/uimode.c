#include "uimode.h"

// Rows are entry points, columns are modes. Settings and the sleep screen delete the grid, so a tile or an icon has
// nothing to land on in them; the model still holds both for the rebuild.
static const ui_action_t TABLE[UIM_ENTRY_COUNT][MODE_COUNT] = {
    // A set applying under settings or a privacy sleep only updates the model; the page is built when the mode ends.
    // (An offline sleep ends instead: ui.c checks uimode_link_wakes first.)
    [UIM_SHOW_SET] = {[MODE_STATUS] = UI_BUILD,
                      [MODE_GRID] = UI_BUILD,
                      [MODE_SETTINGS] = UI_DEFER,
                      [MODE_SLEEP] = UI_DEFER},
    [UIM_SET_TILE] = {[MODE_STATUS] = UI_DROP,
                      [MODE_GRID] = UI_BUILD,
                      [MODE_SETTINGS] = UI_DROP,
                      [MODE_SLEEP] = UI_DROP},
    [UIM_SET_ICON] = {[MODE_STATUS] = UI_DROP,
                      [MODE_GRID] = UI_BUILD,
                      [MODE_SETTINGS] = UI_DROP,
                      [MODE_SLEEP] = UI_DROP},
    // A result shows over settings, but not over a sleep screen: the press is cleared and the result only logged.
    [UIM_SHOW_RESULT] = {[MODE_STATUS] = UI_BUILD,
                         [MODE_GRID] = UI_BUILD,
                         [MODE_SETTINGS] = UI_BUILD,
                         [MODE_SLEEP] = UI_DROP},
    // The list is built when status mode comes back from settings or sleep.
    [UIM_DAEMON_LIST] = {[MODE_STATUS] = UI_BUILD,
                         [MODE_GRID] = UI_BUILD,
                         [MODE_SETTINGS] = UI_DEFER,
                         [MODE_SLEEP] = UI_DEFER},
    // A pairing attempt closes settings. Sleep only runs paired, so a late pairing frame has no place to show.
    [UIM_PAIR_CODE] = {[MODE_STATUS] = UI_BUILD,
                       [MODE_GRID] = UI_BUILD,
                       [MODE_SETTINGS] = UI_BUILD,
                       [MODE_SLEEP] = UI_DROP},
    // The snapshot is in the model already. Only grid mode can have the feed page up, and it checks s_on_feed; the
    // other modes build it from the model when they end (a sleep that ends resets s_on_feed).
    [UIM_FEED] = {[MODE_STATUS] = UI_DROP, [MODE_GRID] = UI_BUILD, [MODE_SETTINGS] = UI_DROP, [MODE_SLEEP] = UI_DROP},
};

ui_action_t uimode_action(ui_entry_t entry, ui_mode_t mode)
{
    if ((unsigned)entry >= UIM_ENTRY_COUNT || (unsigned)mode >= MODE_COUNT) {
        return UI_ACTION_NONE;
    }
    return TABLE[entry][mode];
}

bool uimode_can_sleep(ui_mode_t mode, sleep_reason_t reason, bool paired)
{
    if (!paired) {
        return false;
    }
    return mode == MODE_GRID || (mode == MODE_STATUS && reason == SLEEP_OFFLINE);
}

bool uimode_link_wakes(sleep_reason_t reason)
{
    return reason == SLEEP_OFFLINE;
}
