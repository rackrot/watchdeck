// The settings page: the shell (tab row, Close, idle close, swipe-down close) and the tab table. ui.c owns the mode
// switch around it. Everything runs in the LVGL task or under the LVGL lock.

#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "link.h"
#include "wifi.h"

#define SETTINGS_IDLE_MS 60000 // settings closes after this long without a touch
#define SETTINGS_REFRESH_MS 1000
#define TAB_GAP 8
#define TAB_W 156                // five tabs, Close and the gaps fill the 1024 px row
#define TAB_H SETTINGS_TAB_ROW_H // the buttons fill the row, about 14 mm
#define CLOSE_W 160

// The tabs, in the order of the row. A tab is one row here and its own tab_*.c; the row shows them all, so a new tab
// needs nothing else.
static const settings_tab_t TABS[] = {
    {"Status", tab_status_build, tab_status_tick, tab_status_leave},
    {"Wi-Fi", tab_wifi_build, tab_wifi_tick, tab_wifi_leave},
    {"Storage", tab_storage_build, tab_storage_tick, tab_storage_leave},
    {"Logs", tab_logs_build, tab_logs_tick, tab_logs_leave},
    {"Errors", tab_errors_build, tab_errors_tick, tab_errors_leave},
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
    if (s_page && i != s_tab && !tab_wifi_busy()) {
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
    if (tab_wifi_busy()) {
        return; // a trial connect or a save ends first; leaving would drop its result
    }
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

// Whether something on the page is being read or typed into without touches, so the idle close waits: the license
// viewer, and the Wi-Fi editor, which holds a half-typed password and may wait on a trial connect.
static bool idle_blocked(void)
{
    return ui_licenses_open() || tab_wifi_editing();
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

/* ---- Log lines, shared by the Logs and Errors tabs ---- */

static lv_style_t s_style_error, s_style_warn;
static bool s_styles_ready;
static lv_obj_t *s_line_modal; // on the top layer, NULL while closed
// What the modal's labels point at, so the pool holds none of the text.
static char s_line_head[48];
static char s_line_text[LOGRING_TEXT_MAX + 1];

lv_obj_t *settings_log_line_create(lv_obj_t *parent, int x, int y, int w, int h, lv_event_cb_t on_tap, void *user_data)
{
    if (!s_styles_ready) {
        lv_style_init(&s_style_error);
        lv_style_set_text_color(&s_style_error, lv_palette_lighten(LV_PALETTE_RED, 2));
        lv_style_init(&s_style_warn);
        lv_style_set_text_color(&s_style_warn, lv_palette_lighten(LV_PALETTE_YELLOW, 1));
        s_styles_ready = true;
    }
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_recolor(label, false); // log text can hold '#', which would start recolor markup
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_size(label, w, h);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_clickable(label, true);
    lv_obj_add_event_cb(label, on_tap, LV_EVENT_CLICKED, user_data);
    lv_label_set_text_static(label, "");
    return label;
}

void settings_log_line_set(lv_obj_t *label, const logring_line_t *line)
{
    // The applied level is kept in the label's user data, so a page of the same levels touches no styles.
    const char want = line && (line->level == 'E' || line->level == 'W') ? line->level : 'I';
    const char had = (char)(intptr_t)lv_obj_get_user_data(label);
    if (had != want) {
        lv_obj_remove_style(label, &s_style_error, 0);
        lv_obj_remove_style(label, &s_style_warn, 0);
        if (want == 'E') {
            lv_obj_add_style(label, &s_style_error, 0);
        } else if (want == 'W') {
            lv_obj_add_style(label, &s_style_warn, 0);
        }
        lv_obj_set_user_data(label, (void *)(intptr_t)want);
    }
    lv_label_set_text_static(label, line ? line->text : "");
}

static void line_modal_tapped(lv_event_t *e)
{
    (void)e;
    lv_obj_delete_async(s_line_modal); // from inside its own event
    s_line_modal = NULL;
}

void settings_log_line_open(const logring_line_t *line)
{
    if (s_line_modal) {
        return;
    }
    snprintf(s_line_head, sizeof(s_line_head), "%c  %u.%03u s since boot  #%u", line->level,
             (unsigned)(line->ms / 1000), (unsigned)(line->ms % 1000), (unsigned)line->seq);
    strlcpy(s_line_text, line->text, sizeof(s_line_text));

    s_line_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_line_modal);
    lv_obj_set_size(s_line_modal, GRID_SCREEN_W, GRID_SCREEN_H);
    lv_obj_set_style_bg_color(s_line_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_line_modal, LV_OPA_70, 0);
    lv_obj_set_clickable(s_line_modal, true); // keeps taps off the page below
    lv_obj_add_event_cb(s_line_modal, line_modal_tapped, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_line_modal);
    lv_obj_set_size(card, GRID_SCREEN_W - 80, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, GRID_SCREEN_H - 60, 0);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, false); // the tap lands on the dim layer below

    lv_obj_t *head = lv_label_create(card);
    lv_label_set_recolor(head, false);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_style_text_color(head, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_label_set_text_static(head, s_line_head);

    lv_obj_t *text = lv_label_create(card);
    lv_label_set_recolor(text, false);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(text, lv_color_white(), 0);
    lv_label_set_text_static(text, s_line_text);
}

void settings_log_line_close(void)
{
    if (s_line_modal) {
        lv_obj_delete(s_line_modal);
        s_line_modal = NULL;
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
    wifi_watch_rssi(true);
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
    wifi_watch_rssi(false);
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
