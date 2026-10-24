// The settings page: the shell (tab row, Close, idle close, swipe-down close) and the tab table. ui.c owns the mode
// switch around it. Everything runs in the LVGL task or under the LVGL lock.

#include "settings.h"

#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "link.h"

#define SETTINGS_IDLE_MS 60000 // settings closes after this long without a touch
#define SETTINGS_REFRESH_MS 1000
#define TAB_GAP 8
#define TAB_W 156                // five tabs, Close and the gaps fill the 1024 px row (T2 to T4 add the other four)
#define TAB_H SETTINGS_TAB_ROW_H // the buttons fill the row, about 14 mm
#define CLOSE_W 160

// The tabs, in the order of the row. A tab is one row here and its own tab_*.c; the row shows them all, so a new tab
// needs nothing else.
static const settings_tab_t TABS[] = {
    {"Status", tab_status_build, tab_status_tick, tab_status_leave},
};
#define TAB_COUNT (sizeof(TABS) / sizeof(TABS[0]))

_Static_assert(SETTINGS_PAD + 5 * (TAB_W + TAB_GAP) + CLOSE_W + SETTINGS_PAD <= GRID_SCREEN_W,
               "five tabs and Close fit the row");
_Static_assert(TAB_COUNT <= 5, "the tab row has room for five tabs");

static const char *TAG = "settings";

static lv_obj_t *s_page;    // the whole screen, NULL while closed
static lv_obj_t *s_title;   // "Settings", in the band where the bar would be; only when opened from status mode
static lv_obj_t *s_content; // the shown tab's widgets
static lv_obj_t *s_tab_btns[TAB_COUNT];
static lv_timer_t *s_timer;
static size_t s_tab;      // the tab shown, valid while the page is open
static size_t s_last_tab; // the tab shown last, until reboot
static bool s_swipe_close = true;

void settings_set_swipe_close(bool on)
{
    s_swipe_close = on;
}

static void style_tabs(void)
{
    for (size_t i = 0; i < TAB_COUNT; i++) {
        lv_obj_set_style_bg_color(
            s_tab_btns[i], i == s_tab ? lv_palette_main(LV_PALETTE_BLUE) : lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    }
}

// Delete the shown tab and build tab `i` in its place, so two are never in the pool together.
static void tab_show(size_t i)
{
    if (s_content) {
        if (TABS[s_tab].leave) {
            TABS[s_tab].leave();
        }
        lv_obj_delete(s_content);
    }
    s_tab = s_last_tab = i;
    s_swipe_close = true; // a tab that needs drags (T3's keyboard) turns it off while that view is up
    s_content = lv_obj_create(s_page);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, SETTINGS_CONTENT_W, SETTINGS_CONTENT_H);
    lv_obj_set_pos(s_content, 0, SETTINGS_CONTENT_Y);
    lv_obj_set_scrollable(s_content, false);
    TABS[i].build(s_content);
    style_tabs();
}

// From lv_async_call, outside the tab button's event. The page may have closed since the tap.
static void tab_switch(void *user_data)
{
    const size_t i = (size_t)(intptr_t)user_data;
    if (s_page && i != s_tab) {
        tab_show(i);
    }
}

static void tab_tapped(lv_event_t *e)
{
    lv_async_call(tab_switch, lv_event_get_user_data(e));
}

static void close_tapped(lv_event_t *e)
{
    (void)e;
    ui_settings_close();
}

// Every gesture on the page waits for release, so a swipe that starts on Forget pairing never forgets. A swipe down
// closes.
static void page_gesture(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    lv_indev_wait_release(indev);
    if (s_swipe_close && lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        ui_settings_close();
    }
}

// Whether something on the page is being read or typed into without touches, so the idle close waits. The license
// viewer today; T3's keyboard adds itself here.
static bool idle_blocked(void)
{
    return ui_licenses_open();
}

static void page_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!idle_blocked() && lv_display_get_inactive_time(NULL) >= SETTINGS_IDLE_MS) {
        ESP_LOGI(TAG, "settings idle for %d s, closing", SETTINGS_IDLE_MS / 1000);
        ui_settings_close();
        return;
    }
    if (TABS[s_tab].tick) {
        TABS[s_tab].tick();
    }
}

static lv_obj_t *add_row_button(const char *text, int x, int w, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(s_page);
    lv_obj_set_size(btn, w, TAB_H);
    lv_obj_set_pos(btn, x, GRID_TOP);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

// The page covers the screen on the active screen, so the toast and the result modal on the top layer show over it. In
// grid mode the bar stays on top of it. The text style is set once on the page and inherited, so the tabs add no style
// memory of their own.
void settings_build(ui_mode_t from)
{
    s_page = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, GRID_SCREEN_W, GRID_SCREEN_H);
    lv_obj_set_scrollable(s_page, false);
    lv_obj_set_gesture_bubble(s_page, false);
    lv_obj_add_event_cb(s_page, page_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_text_font(s_page, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_page, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);

    if (from == MODE_STATUS) {
        s_title = lv_label_create(s_page);
        lv_label_set_text(s_title, "Settings");
        lv_obj_set_pos(s_title, SETTINGS_PAD, (BAR_H - SLOT24_H) / 2);
    }

    s_tab = s_last_tab;
    for (size_t i = 0; i < TAB_COUNT; i++) {
        s_tab_btns[i] = add_row_button(TABS[i].name, SETTINGS_PAD + (int)i * (TAB_W + TAB_GAP), TAB_W, tab_tapped,
                                       (void *)(intptr_t)i);
    }
    add_row_button("Close", GRID_SCREEN_W - SETTINGS_PAD - CLOSE_W, CLOSE_W, close_tapped, NULL);

    tab_show(s_last_tab);
    s_timer = lv_timer_create(page_tick, SETTINGS_REFRESH_MS, NULL);
    link_watch_rssi(true);
}

void settings_delete(void)
{
    if (!s_page) {
        return;
    }
    if (TABS[s_tab].leave) {
        TABS[s_tab].leave();
    }
    lv_timer_delete(s_timer);
    s_timer = NULL;
    lv_obj_delete(s_page);
    s_page = s_title = s_content = NULL;
    memset(s_tab_btns, 0, sizeof(s_tab_btns));
    link_watch_rssi(false);
}

bool settings_is_open(void)
{
    return s_page != NULL;
}

void settings_bar_returned(void)
{
    if (s_title) {
        lv_obj_delete(s_title);
        s_title = NULL;
    }
}
