#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "display.h"
#include "font_licenses.h"
#include "fonts.h"
#include "grid.h"
#include "hoststat.h"
#include "icons.h"
#include "link.h"
#include "model.h"
#include "settings.h"
#include "ui_priv.h"

_Static_assert(GRID_SCREEN_W == DISPLAY_H_RES && GRID_SCREEN_H == DISPLAY_V_RES, "grid.h assumes the 1024x600 screen");

#define TOAST_MS 2000
#define RESULT_OK_MS 4000 // a result closes itself after this, or at a tap
#define RESULT_FAIL_MS 10000
#define MAX_PENDING 8
#define PENDING_TTL_MS (11 * 60 * 1000) // longest action timeout (10 min) plus margin
#define FORGET_W 300                    // "Forget pairing", right of the touch status line
#define FORGET_H 40
#define FORGET_HOLD_MS 2000
// "Licenses" sits on the USB line, x 784..1004, y 72..106: 8 px above Forget pairing (y 114..154), so a
// 2 s hold on Forget can't start on it.
#define LICENSES_W 220
#define LICENSES_H 34
#define LICENSES_Y 72
#define SWIPE_LOCK_WARN_MS 100 // a page build holding the model lock longer than this could cost a frame
// A touch that moves further than this in either axis between press and release was a drag, not a tap. 20 px is under a
// third of the smallest cell (8x5 grid, 119x65 px), well above finger jitter on a tap, and under the 50 px at which
// LVGL itself calls a gesture.
#define DRAG_PX 20
// A drag that LVGL missed as a gesture (it zeroes its sum on any read that moves under 3 px) still turns the page when
// it went this far sideways and mostly sideways. On the board (S8, 8x5 grid) short flicks came in at 24 to 50 px; 40 px
// turns the longer of those into page turns and keeps a 20 to 39 px slide as "nothing", neither a press nor a page.
#define DRAG_SWIPE_PX 40
#define DRAG_SWIPE_RATIO 2         // |dx| must exceed this times |dy|
#define ROW_W (DISPLAY_H_RES - 80) // labels and buttons of the pairing views
#define ROW_H 76
#define PAIR_TOP 204 // the pairing views start below the five status lines, which keep their own place
#define STATUS_H 200 // status mode's band of status lines (y 4 to 191), above the pairing views
// A touch that goes this far up, and more than DRAG_SWIPE_RATIO times as far up as sideways, opens settings: a third of
// the screen, 4x DRAG_SWIPE_PX and LVGL's 50 px gesture, so no flick, page turn or tap comes near it.
#define SETTINGS_SWIPE_PX 200
// Status mode's Settings button: right of the Wi-Fi line, as Licenses is right of the USB line.
#define OPEN_SETTINGS_W 220
#define OPEN_SETTINGS_H 34
#define OPEN_SETTINGS_Y 5

// The status bar, 1024 px wide, in Roboto Mono Regular 20 so that every text of a group has a fixed width. Left to
// right: CPU, RAM, GPU, NET, then the date and time. CPU starts BAR_MARGIN from the left edge and the clock ends
// BAR_MARGIN from the right one. The gaps between CPU, RAM, GPU, NET and the clock's area are equal. bar_create
// measures each group's worst case over every unit form and computes them.
#define BAR_MARGIN 12 // px between the first group and the left edge, and between the clock and the right edge
#define BAR_REFRESH_MS 1000

static const char *TAG = "ui";

static bool s_ready; // ui_init succeeded, so the setters and pairing views may take the LVGL lock

// The screen mode, changed only where a mode starts or ends. Settings remembers the mode it replaced, to rebuild it.
static ui_mode_t s_mode = MODE_STATUS;
static ui_mode_t s_settings_from;

// The status texts, kept for status mode and settings. The daemon's has no "Daemon: " and only printable ASCII.
static char s_texts[STATUS_COUNT][STATUS_TEXT_MAX];
static char s_daemon_name[PROTO_NAME_MAX]; // from the last welcome, "" until then and after an unpair

static lv_obj_t *s_status_view; // status mode's lines and buttons, NULL in grid mode
static lv_obj_t *s_status_labels[STATUS_COUNT];
static lv_obj_t *s_forget;        // status mode's Forget pairing, shown while paired
static lv_obj_t *s_open_settings; // status mode's Settings button, hidden during a pairing attempt

static lv_obj_t *s_grid;         // the page on screen, NULL in status mode and while settings is open
static lv_obj_t *s_pair_view;    // unpaired: the daemon list, the pairing code, or its question
static lv_obj_t *s_licenses;     // the font license viewer on the top layer, NULL while closed
static uint32_t s_settings_tick; // when the long swipe that opens settings was recognized

// The status bar of grid mode, NULL in status mode. Its slots, left to right.
typedef enum {
    BAR_STATUS, // the link status text, in place of the four host groups while the link is down
    BAR_CLOCK,
    BAR_CPU, // "CPU   7%  64°C": every host group is a fixed number of monospace cells
    BAR_RAM,
    BAR_GPU,
    BAR_NET, // "▼  1.2M ▲ 340.0k"
    BAR_COUNT,
} bar_slot_t;
static lv_obj_t *s_bar;
static lv_timer_t *s_bar_timer;
static lv_obj_t *s_bar_labels[BAR_COUNT];
LV_FONT_DECLARE(wd_font_bar)                        // Roboto Mono Regular 20, generated from fonts/fonts.json
static lv_font_t s_bar_font;                        // a RAM copy of it with Montserrat 20 behind it for the arrows
static char s_bar_text[BAR_COUNT][STATUS_TEXT_MAX]; // what each slot was last given, as for the settings rows
// The latest `stats` frame and when it came, kept whether or not the bar exists. The clock runs on from it.
static proto_stats_t s_stats;
static int64_t s_stats_at_us;
static bool s_have_stats;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_result;      // a press's result, on the top layer: a backdrop over the grid
static lv_obj_t *s_result_card; // the scrollable panel on the backdrop
static lv_obj_t *s_result_title;
static lv_obj_t *s_result_summary;
static lv_obj_t *s_result_text;
static lv_timer_t *s_result_timer;

// Written and read under the LVGL lock, which also covers the tap callbacks. The s_ids to s_cell_bgs arrays describe
// the page on screen.
static bool s_online;
static bool s_heap_logged;
static uint32_t s_seq;
static struct {
    uint32_t seq;
    uint32_t sent_tick;
    char name[PROTO_ID_MAX]; // the button's label, else its id, so the result names it after the page has changed
} s_pending[MAX_PENDING];
static size_t s_n_pending;
static char s_ids[PROTO_MAX_BUTTONS][PROTO_ID_MAX];
static char s_labels[PROTO_MAX_BUTTONS][PROTO_LABEL_MAX];
static lv_obj_t *s_cells[PROTO_MAX_BUTTONS];      // the grid's buttons
static lv_obj_t *s_cell_icons[PROTO_MAX_BUTTONS]; // their image widgets, NULL until the icon arrives
static uint8_t s_icon_ids[PROTO_MAX_BUTTONS][PROTO_ICON_ID_LEN];
static bool s_has_icon[PROTO_MAX_BUTTONS];
static lv_obj_t *s_cell_values[PROTO_MAX_BUTTONS]; // a tile's value label, NULL for a button without a tile
static uint32_t s_cell_bgs[PROTO_MAX_BUTTONS];     // the configured color, shown while a tile is ok
static size_t s_n_buttons;
static uint32_t s_swipe_tick; // when the latest swipe was recognized; two queued swipes both log the later one's time
static char s_printable[PROTO_TEXT_MAX];
static bool s_paired;
static bool s_attempt_shown; // a pairing attempt's code or question is on screen
static link_daemon_t s_daemons[LINK_MAX_DAEMONS];
static size_t s_n_daemons;
static uint32_t s_forget_pressed_at;

static lv_color_t tile_bg(proto_tile_state_t state, uint32_t ok_bg)
{
    switch (state) {
    case PROTO_TILE_OK:
        return lv_color_hex(ok_bg);
    case PROTO_TILE_BAD:
        return lv_palette_darken(LV_PALETTE_RED, 2);
    default:
        return lv_palette_darken(LV_PALETTE_GREY, 2);
    }
}

// Under the LVGL lock: show status text `i` on its status line, if status mode is on screen.
static void show_status(status_t i)
{
    lv_obj_t *label = s_status_labels[i];
    if (!label) {
        return;
    }
    if (i == STATUS_DAEMON) {
        lv_label_set_text_fmt(label, "Daemon: %s", s_texts[i]);
    } else {
        lv_label_set_text(label, s_texts[i]);
    }
}

static lv_obj_t *add_status(lv_obj_t *parent, status_t i, lv_palette_t palette, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    s_status_labels[i] = label;
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_palette_lighten(palette, 2), 0);
    lv_obj_set_width(label, DISPLAY_H_RES - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, y);
    show_status(i);
    return label;
}

static void toast_hide(lv_timer_t *timer)
{
    lv_obj_set_hidden(s_toast, true);
    lv_timer_pause(timer);
}

static void create_toast(void)
{
    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 16, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_width(s_toast, DISPLAY_H_RES - 120);
    lv_label_set_long_mode(s_toast, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_hidden(s_toast, true);
    s_toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
    lv_timer_pause(s_toast_timer);
}

// Under the LVGL lock.
static void show_toast(lv_color_t bg, const char *text)
{
    lv_obj_set_style_bg_color(s_toast, bg, 0);
    lv_label_set_text(s_toast, text);
    lv_obj_set_hidden(s_toast, false);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}

/* ---- Result modal, all under the LVGL lock ---- */

static void result_hide(void)
{
    lv_obj_set_hidden(s_result, true);
    lv_timer_pause(s_result_timer);
}

static void result_timed_out(lv_timer_t *timer)
{
    result_hide();
}

// A tap anywhere closes it. A drag on the card scrolls the output instead.
static void result_tapped(lv_event_t *e)
{
    result_hide();
}

static lv_obj_t *add_result_label(const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(s_result_card);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

static void create_result(void)
{
    s_result = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_result);
    lv_obj_set_size(s_result, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_style_bg_color(s_result, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_result, LV_OPA_70, 0);
    lv_obj_set_clickable(s_result, true); // and so keeps taps off the grid below
    lv_obj_add_event_cb(s_result, result_tapped, LV_EVENT_CLICKED, NULL);

    s_result_card = lv_obj_create(s_result);
    lv_obj_set_size(s_result_card, DISPLAY_H_RES - 80, DISPLAY_V_RES - 60);
    lv_obj_center(s_result_card);
    lv_obj_set_style_bg_color(s_result_card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(s_result_card, 0, 0);
    lv_obj_set_style_radius(s_result_card, 16, 0);
    lv_obj_set_style_pad_all(s_result_card, 24, 0);
    lv_obj_set_style_pad_row(s_result_card, 12, 0);
    lv_obj_set_flex_flow(s_result_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_event_bubble(s_result_card, true);

    s_result_title = add_result_label(&lv_font_montserrat_48);
    s_result_summary = add_result_label(&lv_font_montserrat_32);
    s_result_text = add_result_label(&lv_font_montserrat_24);
    lv_obj_set_hidden(s_result, true);
    s_result_timer = lv_timer_create(result_timed_out, RESULT_OK_MS, NULL);
    lv_timer_pause(s_result_timer);
}

// Copy `src`, turning anything but printable ASCII and newlines into '?', since the font has no
// other glyphs. The daemon sends only those, so this only guards against a buggy one.
static void copy_printable(char *dst, size_t dst_len, const char *src)
{
    size_t i = 0;
    for (; src[i] && i + 1 < dst_len; i++) {
        char c = src[i];
        dst[i] = c == '\n' || (c >= ' ' && c <= '~') ? c : '?';
    }
    dst[i] = '\0';
}

// `button` names the pressed button: its label when it had one, else its id.
static void show_result(const proto_result_t *r, const char *button)
{
    const char *verdict = r->ok ? "OK" : "Failed";
    char name[PROTO_ID_MAX];
    copy_printable(name, sizeof(name), button);
    if (name[0]) {
        lv_label_set_text_fmt(s_result_title, "%s: %s", name, verdict);
    } else {
        lv_label_set_text(s_result_title, verdict);
    }
    lv_palette_t palette = r->ok ? LV_PALETTE_GREEN : LV_PALETTE_RED;
    lv_obj_set_style_text_color(s_result_title, lv_palette_lighten(palette, 2), 0);

    copy_printable(s_printable, PROTO_SUMMARY_MAX, r->summary);
    if (!s_printable[0]) {
        lv_label_set_text_fmt(s_result_summary, "code %d, %u ms", r->code, (unsigned)r->ms);
    } else if (r->ms > 0) {
        lv_label_set_text_fmt(s_result_summary, "%s, %u ms", s_printable, (unsigned)r->ms);
    } else {
        lv_label_set_text(s_result_summary, s_printable);
    }
    copy_printable(s_printable, sizeof(s_printable), r->text);
    lv_label_set_text(s_result_text, s_printable);
    lv_obj_set_hidden(s_result_text, !s_printable[0]);

    lv_obj_scroll_to_y(s_result_card, 0, LV_ANIM_OFF);
    lv_obj_set_hidden(s_result, false);
    lv_timer_set_period(s_result_timer, r->ok ? RESULT_OK_MS : RESULT_FAIL_MS);
    lv_timer_reset(s_result_timer);
    lv_timer_resume(s_result_timer);
}

// A short tap only explains itself, so a stray touch can't wipe the pairing.
static void forget_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_forget_pressed_at = lv_tick_get();
    } else if (lv_tick_elaps(s_forget_pressed_at) >= FORGET_HOLD_MS) {
        ESP_LOGI(TAG, "forget pairing");
        link_forget();
    } else {
        show_toast(lv_palette_darken(LV_PALETTE_GREY, 3), "Hold Forget pairing for 2 s to unpair this device.");
    }
}

lv_obj_t *ui_add_forget(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, FORGET_W, FORGET_H);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_add_event_cb(btn, forget_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, forget_event, LV_EVENT_RELEASED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_label_set_text(label, "Forget pairing");
    lv_obj_center(label);
    return btn;
}

/* ---- Font license viewer, all under the LVGL lock ---- */

static void licenses_close(lv_event_t *e)
{
    lv_obj_delete_async(s_licenses); // from inside the Close button's own callback
    s_licenses = NULL;
}

// A full-screen page: Close on top, then the license text, which a drag scrolls.
static void licenses_open(lv_event_t *e)
{
    if (s_licenses) {
        return;
    }
    s_licenses = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_licenses);
    lv_obj_set_size(s_licenses, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_style_bg_color(s_licenses, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_licenses, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_licenses, 12, 0);
    lv_obj_set_style_pad_row(s_licenses, GRID_GAP, 0);
    lv_obj_set_flex_flow(s_licenses, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(s_licenses, false);

    lv_obj_t *close = lv_button_create(s_licenses);
    lv_obj_set_size(close, 240, 56);
    lv_obj_add_event_cb(close, licenses_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_obj_set_style_text_font(close_label, &lv_font_montserrat_32, 0);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    lv_obj_t *hint = lv_label_create(s_licenses);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hint, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_label_set_text(hint, "Drag to scroll");

    lv_obj_t *body = lv_obj_create(s_licenses);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_t *text = lv_label_create(body);
    lv_obj_set_width(text, LV_PCT(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(text, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(text, lv_color_white(), 0);
    lv_label_set_text_static(text, font_licenses_text()); // lives in flash, no copy
}

bool ui_licenses_open(void)
{
    return s_licenses != NULL;
}

lv_obj_t *ui_add_licenses_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, LICENSES_W, LICENSES_H);
    lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_add_event_cb(btn, licenses_open, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_label_set_text(label, "Licenses");
    lv_obj_center(label);
    return btn;
}

/* ---- Status mode, all under the LVGL lock ---- */

static void settings_enter(void *user_data);

// The Settings button is hidden while a pairing code or the "Paired?" question is on screen, so settings never covers
// an attempt. s_attempt_shown is set and cleared in the pairing views below.
static void show_open_settings(void)
{
    if (s_open_settings) {
        lv_obj_set_hidden(s_open_settings, s_attempt_shown);
    }
}

// From lv_async_call, since the tap's own widget goes with the status view.
static void open_settings_tapped(lv_event_t *e)
{
    (void)e;
    s_settings_tick = lv_tick_get();
    lv_async_call(settings_enter, (void *)(intptr_t)MODE_STATUS);
}

// Build status mode's widgets from the kept texts, unless they are on screen already.
static void status_mode_start(void)
{
    if (s_status_view) {
        return;
    }
    s_status_view = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_status_view);
    lv_obj_set_size(s_status_view, DISPLAY_H_RES, STATUS_H);
    lv_obj_set_scrollable(s_status_view, false);
    lv_obj_set_clickable(s_status_view, false);
    lv_obj_t *wifi = add_status(s_status_view, STATUS_WIFI, LV_PALETTE_CYAN, 4);
    lv_obj_set_width(wifi, DISPLAY_H_RES - 60 - OPEN_SETTINGS_W); // the Settings button sits right of it
    add_status(s_status_view, STATUS_SD, LV_PALETTE_LIGHT_GREEN, 42);
    lv_obj_t *usb = add_status(s_status_view, STATUS_USB, LV_PALETTE_AMBER, 80);
    lv_obj_set_width(usb, DISPLAY_H_RES - 60 - LICENSES_W); // "USB: error (ESP_ERR_NOT_SUPPORTED)" is 655 px of 744
    lv_obj_t *touch = add_status(s_status_view, STATUS_TOUCH, LV_PALETTE_PINK, 118);
    lv_obj_set_width(touch, DISPLAY_H_RES - 60 - FORGET_W); // the touch text is short
    add_status(s_status_view, STATUS_DAEMON, LV_PALETTE_PURPLE, 156);
    s_forget = ui_add_forget(s_status_view);
    lv_obj_align(s_forget, LV_ALIGN_TOP_RIGHT, -20, 114);
    lv_obj_set_hidden(s_forget, !s_paired);
    lv_obj_align(ui_add_licenses_button(s_status_view), LV_ALIGN_TOP_RIGHT, -20, LICENSES_Y);

    s_open_settings = lv_button_create(s_status_view);
    lv_obj_set_size(s_open_settings, OPEN_SETTINGS_W, OPEN_SETTINGS_H);
    lv_obj_align(s_open_settings, LV_ALIGN_TOP_RIGHT, -20, OPEN_SETTINGS_Y);
    lv_obj_set_style_bg_color(s_open_settings, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_add_event_cb(s_open_settings, open_settings_tapped, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(s_open_settings);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_label_set_text(label, "Settings");
    lv_obj_center(label);
    show_open_settings();
}

// The first set applied: delete status mode's widgets, so grid mode doesn't carry them in the LVGL pool. A hold on its
// Forget pairing ends here; LVGL waits for that touch's release.
static void status_mode_end(void)
{
    if (!s_status_view) {
        return;
    }
    lv_obj_delete(s_status_view);
    s_status_view = NULL;
    memset(s_status_labels, 0, sizeof(s_status_labels));
    s_forget = NULL;
    s_open_settings = NULL;
}

// Drop presses whose result never came, so they cannot fill the table forever.
static void expire_pending(void)
{
    for (size_t i = 0; i < s_n_pending;) {
        if (lv_tick_elaps(s_pending[i].sent_tick) > PENDING_TTL_MS) {
            ESP_LOGW(TAG, "press seq %u expired without a result", (unsigned)s_pending[i].seq);
            s_pending[i] = s_pending[--s_n_pending];
        } else {
            i++;
        }
    }
}

static void swipe_page(void *user_data);

// Where the one live touch began, where it was last seen, and its largest excursion from the start on each axis. Only
// the LVGL task touches these. The release read can repeat a stale point, so the end comes from PRESSING reads.
static lv_point_t s_touch_start, s_touch_end;
static int s_touch_max_dx, s_touch_max_dy;
static uint32_t s_touch_samples, s_touch_start_tick;

// Reads per touch show whether a fast flick is being sampled often enough to see it move. Debug level: at 115200 baud
// an INFO line costs about 10 ms in the LVGL task on every press.
static void touch_log(const char *what)
{
    ESP_LOGD(TAG, "touch on %s: %u samples in %u ms, max %d,%d net %d,%d", what, (unsigned)s_touch_samples,
             (unsigned)lv_tick_elaps(s_touch_start_tick), s_touch_max_dx, s_touch_max_dy,
             s_touch_end.x - s_touch_start.x, s_touch_end.y - s_touch_start.y);
}

// On the grid and each of its cells. A long swipe up opens settings the moment it crosses SETTINGS_SWIPE_PX, before the
// finger lifts. The wait for release stops PRESSING for this touch and makes its release press nothing, and the grid
// goes from lv_async_call, outside the event of the widget being pressed.
static void touch_track(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_touch_start = p;
        s_touch_max_dx = s_touch_max_dy = 0;
        s_touch_samples = 0;
        s_touch_start_tick = lv_tick_get();
    }
    s_touch_samples++;
    s_touch_end = p;
    s_touch_max_dx = LV_MAX(s_touch_max_dx, LV_ABS(p.x - s_touch_start.x));
    s_touch_max_dy = LV_MAX(s_touch_max_dy, LV_ABS(p.y - s_touch_start.y));

    const int up = s_touch_start.y - p.y;
    if (code == LV_EVENT_PRESSING && up >= SETTINGS_SWIPE_PX && up > DRAG_SWIPE_RATIO * LV_ABS(p.x - s_touch_start.x)) {
        lv_indev_wait_release(indev);
        touch_log("long swipe up");
        s_settings_tick = lv_tick_get();
        lv_async_call(settings_enter, (void *)(intptr_t)MODE_GRID);
    }
}

// Called where a click would act. Returns true when the touch was a drag, so no press may follow. A gesture LVGL
// recognised never gets here: lv_indev_wait_release makes indev_proc_release skip RELEASED and CLICKED for that touch
// (lv_indev.c:1518-1530, 1551-1575), so one touch steps at most once.
static bool drag_consumed(const char *what)
{
    const int dx = s_touch_end.x - s_touch_start.x;
    const int dy = s_touch_end.y - s_touch_start.y;
    // The largest excursion, not the net move, so a slide that returns to its start is still no tap.
    if (s_touch_max_dx <= DRAG_PX && s_touch_max_dy <= DRAG_PX) {
        return false;
    }
    ESP_LOGI(TAG, "drag on %s ignored", what);
    touch_log(what);
    if (LV_ABS(dx) >= DRAG_SWIPE_PX && LV_ABS(dx) > DRAG_SWIPE_RATIO * LV_ABS(dy)) {
        s_swipe_tick = lv_tick_get();
        lv_async_call(swipe_page, (void *)(intptr_t)(dx < 0 ? 1 : -1));
    }
    return true;
}

static void grid_click(lv_event_t *e)
{
    (void)e;
    drag_consumed("grid");
}

static void cell_event(lv_event_t *e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    const char *id = s_ids[i];
    if (drag_consumed(id)) {
        return;
    }
    if (!s_online) {
        ESP_LOGW(TAG, "tap on %s ignored, daemon offline", id);
        return;
    }
    expire_pending();
    if (s_n_pending == MAX_PENDING) {
        ESP_LOGW(TAG, "tap on %s ignored, %d presses pending", id, MAX_PENDING);
        return;
    }
    uint32_t seq = ++s_seq;
    if (link_press(id, seq) == ESP_OK) {
        typeof(s_pending[0]) *p = &s_pending[s_n_pending++];
        p->seq = seq;
        p->sent_tick = lv_tick_get();
        strlcpy(p->name, s_labels[i][0] ? s_labels[i] : id, sizeof(p->name));
        ESP_LOGI(TAG, "press %s seq %u", id, (unsigned)seq);
        touch_log(id);
    }
}

esp_err_t ui_init(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "no display");
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    strlcpy(s_texts[STATUS_WIFI], "Wi-Fi: starting", STATUS_TEXT_MAX);
    strlcpy(s_texts[STATUS_SD], "SD: checking", STATUS_TEXT_MAX);
    strlcpy(s_texts[STATUS_USB], "USB: starting", STATUS_TEXT_MAX);
    strlcpy(s_texts[STATUS_TOUCH], "Touch: starting", STATUS_TEXT_MAX);
    strlcpy(s_texts[STATUS_DAEMON], "waiting for Wi-Fi", STATUS_TEXT_MAX);
    status_mode_start();
    create_toast();
    create_result();
    s_ready = true;
    lvgl_port_unlock();
    return ESP_OK;
}

// A swipe anywhere on the grid, whatever it started on. The release must press nothing, an outward swipe at either end
// included. The rebuild waits for lv_async_call, so the pressed widget isn't deleted inside its own event. An upward
// gesture doesn't wait, since that would end PRESSING (lv_indev.c:1293) at LVGL's 50 px, before touch_track can see a
// long swipe up. Its release still presses nothing, because drag_consumed refuses any touch that moved over DRAG_PX.
static void grid_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP) {
        return;
    }
    lv_indev_wait_release(indev);
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        s_swipe_tick = lv_tick_get();
        lv_async_call(swipe_page, (void *)(intptr_t)(dir == LV_DIR_LEFT ? 1 : -1));
    }
}

// Replace the grid with page `l` of a set of `pages`. With 2 or more pages the cells give up a strip at the bottom for
// "Page x of y". The grid keeps its full height either way, and never scrolls.
static void build_grid(const proto_layout_t *l, uint8_t pages)
{
    if (s_grid) {
        lv_obj_delete(s_grid);
    }
    s_grid = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_grid);
    // Children bubble gestures by default (lv_obj.c), so a swipe that starts on a button lands here.
    lv_obj_set_gesture_bubble(s_grid, false);
    lv_obj_add_event_cb(s_grid, grid_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_grid, touch_track, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_grid, touch_track, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_grid, grid_click, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(s_grid, DISPLAY_H_RES, DISPLAY_V_RES - GRID_TOP);
    lv_obj_set_pos(s_grid, 0, GRID_TOP);
    lv_obj_set_scrollable(s_grid, false);

    // The daemon sizes icons to what is left (icons.rs box_px), so both sides use grid_geometry.
    const grid_geometry_t geo = grid_geometry(l->cols, l->rows, pages >= 2);
    if (pages >= 2) {
        // A child of the grid, so it dims with it and goes with it. Not clickable, so a press on it lands on the grid.
        lv_obj_t *pager = lv_label_create(s_grid);
        lv_obj_set_size(pager, DISPLAY_H_RES, SLOT24_H);
        lv_obj_set_pos(pager, 0, geo.grid_h);
        lv_obj_set_style_text_align(pager, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(pager, wd_font(l->font, WD_SLOT_24), 0);
        lv_obj_set_style_text_color(pager, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_label_set_text_fmt(pager, "Page %u of %u", (unsigned)l->page + 1, (unsigned)pages);
    }
    const int cell_w = geo.cell_w;
    const int cell_h = geo.cell_h;
    const int line_h = SLOT32_H;
    const int label_lines = geo.label_lines;
    const int icon_side = geo.icon_side;
    ESP_LOGI(TAG, "cells %dx%d, icon box %d px, labels %d line(s)", cell_w, cell_h, icon_side, label_lines);
    s_n_buttons = l->n_buttons;
    for (size_t i = 0; i < l->n_buttons; i++) {
        const proto_button_t *b = &l->buttons[i];
        strlcpy(s_ids[i], b->id, sizeof(s_ids[i]));
        copy_printable(s_labels[i], sizeof(s_labels[i]), b->label);

        s_has_icon[i] = b->has_icon;
        memcpy(s_icon_ids[i], b->icon, sizeof(s_icon_ids[i]));
        s_cell_icons[i] = NULL;
        s_cell_values[i] = NULL;
        s_cell_bgs[i] = b->bg;

        lv_obj_t *btn = lv_button_create(s_grid);
        s_cells[i] = btn;
        lv_obj_set_size(btn, cell_w, cell_h);
        lv_obj_set_pos(btn, GRID_GAP + b->col * (cell_w + GRID_GAP), b->row * (cell_h + GRID_GAP));
        // A cell whose icon and label overflow it by a pixel would scroll on a vertical drag instead of sending
        // CLICKED.
        lv_obj_set_scrollable(btn, false);
        // A tile is grey until its first value arrives.
        lv_obj_set_style_bg_color(btn, b->tile ? tile_bg(PROTO_TILE_UNKNOWN, b->bg) : lv_color_hex(b->bg), 0);
        lv_obj_add_event_cb(btn, touch_track, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(btn, touch_track, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(btn, cell_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        // The icon, once it arrives, goes above the label.
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(btn, ICON_PAD, 0);
        lv_obj_set_style_pad_row(btn, ICON_GAP, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_width(label, cell_w - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        if (b->has_icon) {
            // Fixed height, so a long label ends in dots instead of pushing the icon out of the button.
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_height(label, label_lines * line_h);
        }
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, wd_font(b->label_font, WD_SLOT_32), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(b->fg), 0);
        lv_label_set_text(label, s_labels[i]);

        if (b->tile) {
            // The label shrinks to one line of 24 pt and the value goes under it. With an icon on a grid of
            // 5 rows (one label line) there is room for the icon and the value only, so the label goes.
            lv_obj_set_style_text_font(label, wd_font(b->label_font, WD_SLOT_24), 0);
            lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
            lv_obj_set_height(label, SLOT24_H);
            lv_obj_set_hidden(label, b->has_icon && label_lines == 1);

            const bool big = cell_h >= TILE_BIG_MIN_H;
            const lv_font_t *font = wd_font(b->value_font, big ? WD_SLOT_32 : WD_SLOT_24);
            lv_obj_t *value = lv_label_create(btn);
            s_cell_values[i] = value;
            lv_obj_set_width(value, cell_w - 16);
            lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
            lv_obj_set_height(value, big ? SLOT32_H : SLOT24_H);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(value, font, 0);
            lv_obj_set_style_text_color(value, lv_color_hex(b->fg), 0);
            lv_label_set_text(value, "");
        }
    }
}

// Under the LVGL lock: show `dsc` above the label of button `i` on screen.
static void show_icon(size_t i, const lv_image_dsc_t *dsc)
{
    if (!s_cell_icons[i]) {
        s_cell_icons[i] = lv_image_create(s_cells[i]);
        lv_obj_move_to_index(s_cell_icons[i], 0);
    }
    lv_image_set_src(s_cell_icons[i], dsc);
}

// Under the LVGL lock: show a tile's value on tile button `i` on screen.
static void show_tile(size_t i, const proto_tile_t *t)
{
    char text[PROTO_TILE_MAX];
    copy_printable(text, sizeof(text), t->text);
    for (char *c = text; *c; c++) {
        if (*c == '\n') {
            *c = ' '; // one line
        }
    }
    lv_label_set_text(s_cell_values[i], text);
    lv_obj_set_style_bg_color(s_cells[i], tile_bg(t->state, s_cell_bgs[i]), 0);
}

// Under the LVGL lock, then the model lock: build the page shown from the model's set, with the icons and tile values
// the model already holds. Leaves s_online alone, and dims the page while the link is down. Returns its index.
static uint8_t build_page(void)
{
    const uint8_t shown = model_shown();
    build_grid(model_page(shown), model_pages());
    for (size_t i = 0; i < s_n_buttons; i++) {
        const lv_image_dsc_t *dsc = s_has_icon[i] ? icons_find(s_icon_ids[i]) : NULL;
        if (dsc) {
            show_icon(i, dsc);
        }
        const proto_tile_t *t = s_cell_values[i] ? model_tile(s_ids[i]) : NULL;
        if (t) {
            show_tile(i, t);
        }
    }
    lv_obj_set_style_opa(s_grid, s_online ? LV_OPA_COVER : LV_OPA_30, 0);
    return shown;
}

// Runs in the LVGL task, so under the LVGL lock. `user_data` is the direction (+1 next, -1 previous), resolved against
// the page shown now, since a new set may have applied since the swipe. Sends nothing to the link, and works offline.
static void swipe_page(void *user_data)
{
    const int dir = (int)(intptr_t)user_data;
    model_lock();
    const uint32_t locked_at = lv_tick_get();
    if (!s_grid || model_pages() == 0 || !model_step(dir)) {
        model_unlock();
        return; // no set, or an outward swipe at an end
    }
    const uint8_t shown = build_page();
    const uint8_t pages = model_pages();
    model_unlock();
    const uint32_t held_ms = lv_tick_elaps(locked_at);
    ESP_LOGI(TAG, "page %u/%u shown in %u ms", shown + 1, pages, (unsigned)lv_tick_elaps(s_swipe_tick));
    if (held_ms > SWIPE_LOCK_WARN_MS) {
        ESP_LOGW(TAG, "page build held the model lock %u ms, copy the page out under it", (unsigned)held_ms);
    }
}

// One line to compare memory at the same point across runs: the LVGL pool, then the internal and PSRAM heaps.
void ui_log_heap(const char *when)
{
    lv_mem_monitor_t mem;
    lvgl_port_lock(0);
    lv_mem_monitor(&mem);
    lvgl_port_unlock();
    ESP_LOGI(TAG,
             "LVGL heap (%s): %u of %u bytes used now, %u at most, %u%% fragmented, largest free %u; internal free %u "
             "min %u largest %u; PSRAM free %u min %u largest %u",
             when, (unsigned)(mem.total_size - mem.free_size), (unsigned)mem.total_size, (unsigned)mem.max_used,
             (unsigned)mem.frag_pct, (unsigned)mem.free_biggest_size,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* ---- Settings: the mode switch. The page and its tabs are in settings.c ---- */

// Put `text` on `label` unless `cache` says it already shows it. The cache is the source string: a LONG_DOT label edits
// the text it holds, so comparing against lv_label_get_text would reset an overflowing row on every refresh.
void ui_set_label(lv_obj_t *label, char *cache, size_t cache_size, const char *text)
{
    if (strcmp(cache, text) != 0) {
        strlcpy(cache, text, cache_size);
        lv_label_set_text(label, text);
    }
}

ui_mode_t ui_mode(void)
{
    return s_mode;
}

const char *ui_status_text(status_t i)
{
    return s_texts[i];
}

const char *ui_daemon_name(void)
{
    return s_daemon_name;
}

static void show_daemon_list(void);
static void clear_pair_view(void);

// Delete the page and bring back the mode it replaced: grid mode rebuilds the model's page, dimmed while the link is
// down; status mode rebuilds its lines and, if unpaired, the daemon list. Deletes before building, so the two are never
// in the LVGL pool together.
static void settings_leave(void)
{
    const uint32_t start = lv_tick_get();
    settings_delete();
    s_mode = s_settings_from;
    if (s_mode == MODE_GRID) {
        model_lock();
        if (model_pages() > 0) {
            build_page();
        }
        model_unlock();
    } else {
        status_mode_start();
        if (!s_paired) {
            show_daemon_list();
        }
    }
    ESP_LOGI(TAG, "settings closed, %s rebuilt in %u ms", s_mode == MODE_GRID ? "page" : "status mode",
             (unsigned)lv_tick_elaps(start));
    ui_log_heap(s_mode == MODE_GRID ? "page built" : "status built");
}

// Runs from lv_async_call, outside the event of any widget it deletes. A Close tap, a swipe down and the idle timer may
// all queue it, so a second call finds nothing to close.
static void settings_close_cb(void *user_data)
{
    (void)user_data;
    if (s_mode == MODE_SETTINGS) {
        settings_leave();
    }
}

void ui_settings_close(void)
{
    lv_async_call(settings_close_cb, NULL);
}

// Runs from lv_async_call after a long swipe up or a tap on status mode's Settings, outside the event of the widget
// that went with the mode. `user_data` is the mode it opened from. Deletes that mode's widgets before building the
// page. An unpair or a pairing attempt since the tap left a different mode, or one not to cover, and then nothing
// opens.
static void settings_enter(void *user_data)
{
    const ui_mode_t from = (ui_mode_t)(intptr_t)user_data;
    if (s_mode != from || (from == MODE_STATUS && s_attempt_shown)) {
        return;
    }
    if (from == MODE_GRID) {
        lv_obj_delete(s_grid);
        s_grid = NULL;
        s_n_buttons = 0; // so ui_set_icon and ui_set_tile find no cells while the grid is gone
    } else {
        status_mode_end();
        clear_pair_view();
    }
    s_settings_from = from;
    s_mode = MODE_SETTINGS;
    settings_build(from);
    ESP_LOGI(TAG, "settings open %u ms after the %s", (unsigned)lv_tick_elaps(s_settings_tick),
             from == MODE_GRID ? "swipe" : "tap");
    ui_log_heap("settings open");
}

/* ---- Status bar, all in the LVGL task or under the LVGL lock ---- */

static void bar_set(bar_slot_t slot, const char *text)
{
    ui_set_label(s_bar_labels[slot], s_bar_text[slot], STATUS_TEXT_MAX, text);
}

// The text of a host group. Values sit right-aligned in fixed cells, so in a monospace font nothing moves within a
// unit, and a value past a threshold is wrapped in recolor markup (the four labels have recolor on).
static void bar_group_text(bar_slot_t slot, const proto_stats_t *stats, bool fresh, char *buf, size_t size)
{
    switch (slot) {
    case BAR_CPU:
        hoststat_group_cpu(buf, size, stats, fresh);
        break;
    case BAR_RAM:
        hoststat_group_mem(buf, size, stats, fresh);
        break;
    case BAR_GPU:
        hoststat_group_gpu(buf, size, stats, fresh);
        break;
    default:
        hoststat_group_net(buf, size, stats, fresh, LV_SYMBOL_DOWN, LV_SYMBOL_UP);
        break;
    }
}

// Show the latest stats in the slots. While the link is down the status text takes the four host slots' place and the
// clock stays. The host values show "--" once the latest frame is stale (hoststat_fresh), or when it lacked them.
static void bar_refresh(void)
{
    if (!s_bar) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    char text[HOSTSTAT_GROUP_MAX];
    if (s_have_stats) {
        // time and utc_offset are the frame's, and the crystal carries the clock on from there.
        hoststat_clock(
            text, sizeof(text),
            hoststat_local_time(s_stats.time, s_stats.has_utc_offset ? s_stats.utc_offset : 0, now - s_stats_at_us));
    } else {
        strlcpy(text, "--:--", sizeof(text));
    }
    bar_set(BAR_CLOCK, text);

    if (!s_online) {
        bar_set(BAR_STATUS, s_texts[STATUS_DAEMON]);
        for (bar_slot_t slot = BAR_CPU; slot < BAR_COUNT; slot++) {
            bar_set(slot, "");
        }
        return;
    }
    bar_set(BAR_STATUS, "");
    const bool fresh = hoststat_fresh(s_have_stats, now - s_stats_at_us);
    for (bar_slot_t slot = BAR_CPU; slot < BAR_COUNT; slot++) {
        bar_group_text(slot, &s_stats, fresh, text, sizeof(text));
        bar_set(slot, text);
    }
}

static void bar_tick(lv_timer_t *timer)
{
    (void)timer;
    bar_refresh();
}

static int bar_text_w(const char *text)
{
    lv_point_t size;
    // The recolor flag makes a "#rrggbb text#" measure as just its text, the way the labels draw it.
    lv_text_get_size(&size, text, &s_bar_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_RECOLOR);
    return size.x;
}

// The widest a host group gets: every value at its longest, in every unit form, and with a color on every value. The
// same frame without colors must measure the same, or the markup would take width and the label would reach its dots,
// which could cut a "#rrggbb ...#" in two.
static int bar_group_w(bar_slot_t slot)
{
    proto_stats_t stats = {
        .has_cpu = true,
        .cpu = 100,
        .has_mem = true,
        .mem_used = 1,
        .mem_total = 1,
        .has_net = true,
        .net_rx = UINT64_MAX,
        .net_tx = UINT64_MAX,
        .has_gpu = true,
        .gpu = 100,
        .has_cpu_temp = true,
        .cpu_temp = 150,
        .has_gpu_temp = true,
        .gpu_temp = 150,
    };
    int widest = 0;
    char text[HOSTSTAT_GROUP_MAX];
    for (int temp = PROTO_TEMP_C; temp <= PROTO_TEMP_F; temp++) {
        for (int net = PROTO_NET_SI; net <= PROTO_NET_BITS; net++) {
            stats.temp_unit = temp;
            stats.net_unit = net;
            stats.colors_mask = (1 << PROTO_COLOR_COUNT) - 1;
            bar_group_text(slot, &stats, true, text, sizeof(text));
            const int colored = bar_text_w(text);
            stats.colors_mask = 0;
            bar_group_text(slot, &stats, true, text, sizeof(text));
            const int plain = bar_text_w(text);
            if (colored != plain) {
                ESP_LOGW(TAG, "bar slot overflow: color markup takes %d px in slot %d", colored - plain, (int)slot);
            }
            widest = LV_MAX(widest, LV_MAX(colored, plain));
        }
    }
    return widest;
}

// Grid mode starts: the bar on top, drawn from the stats and status already kept.
static void bar_create(void)
{
    if (s_bar) {
        return;
    }
    // The bar font (fonts.json, chrome) has ASCII and the degree sign. The arrows come from Montserrat 20, the size
    // nearest its digits.
    s_bar_font = wd_font_bar;
    s_bar_font.fallback = &lv_font_montserrat_20;

    // In a monospace font every ASCII character is as wide as the widest, so a worst case is its longest string. Only
    // the degree sign and the arrows come from Montserrat, and the strings below contain both arrows and the sign.
    // (lv_text_get_size drops a trailing space, so the space is measured between two digits.)
    if (bar_text_w("0") != bar_text_w("W") || bar_text_w("0 0") != 3 * bar_text_w("0")) {
        ESP_LOGW(TAG, "bar slot overflow: the bar font is not monospace, so its worst cases are not sized");
    }
    // Each group's worst case over every unit form ("CPU 100% 302°F", "▼999.9Mi ▲999.9Mi").
    int w[BAR_COUNT] = {0};
    w[BAR_CLOCK] = bar_text_w("Wed 24 May 04:44");
    for (bar_slot_t slot = BAR_CPU; slot < BAR_COUNT; slot++) {
        w[slot] = bar_group_w(slot);
    }
    // CPU starts at BAR_MARGIN and the clock ends BAR_MARGIN from the right edge. The four gaps between CPU, RAM, GPU,
    // NET and the clock's area are equal, within 1 px: the remainder goes one px each to the first gaps.
    const int clock_x = DISPLAY_H_RES - BAR_MARGIN - w[BAR_CLOCK];
    const int free_px = clock_x - BAR_MARGIN - w[BAR_CPU] - w[BAR_RAM] - w[BAR_GPU] - w[BAR_NET];
    if (free_px < 0) {
        ESP_LOGW(TAG, "bar slot overflow: the groups need %d px more than the bar has", -free_px);
    }
    const int gap = free_px > 0 ? free_px / 4 : 0;
    const int spare = free_px > 0 ? free_px % 4 : 0;
    int x[BAR_COUNT] = {0};
    x[BAR_STATUS] = BAR_MARGIN;
    x[BAR_CLOCK] = clock_x;
    x[BAR_CPU] = BAR_MARGIN;
    x[BAR_RAM] = x[BAR_CPU] + w[BAR_CPU] + gap + (0 < spare);
    x[BAR_GPU] = x[BAR_RAM] + w[BAR_RAM] + gap + (1 < spare);
    x[BAR_NET] = x[BAR_GPU] + w[BAR_GPU] + gap + (2 < spare);
    w[BAR_STATUS] = clock_x - 2 * BAR_MARGIN; // the link status text spans the group area
    // The text is as tall as the bar font's line, centered in the bar.
    const int text_h = s_bar_font.line_height;
    const int text_y = (BAR_H - text_h) / 2;
    ESP_LOGI(TAG,
             "bar groups (x/width): cpu %d/%d ram %d/%d gpu %d/%d net %d/%d clock %d/%d, gap %d (+%d spare), margins "
             "%d left %d right, text y %d h %d",
             x[BAR_CPU], w[BAR_CPU], x[BAR_RAM], w[BAR_RAM], x[BAR_GPU], w[BAR_GPU], x[BAR_NET], w[BAR_NET],
             x[BAR_CLOCK], w[BAR_CLOCK], gap, spare, x[BAR_CPU], DISPLAY_H_RES - x[BAR_CLOCK] - w[BAR_CLOCK], text_y,
             text_h);

    s_bar = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, DISPLAY_H_RES, BAR_H);
    lv_obj_set_pos(s_bar, 0, 0);
    lv_obj_set_scrollable(s_bar, false);
    lv_obj_set_clickable(s_bar, false);
    // Set once here and inherited, so the labels add no style memory of their own.
    lv_obj_set_style_text_font(s_bar, &s_bar_font, 0);
    lv_obj_set_style_text_color(s_bar, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
    memset(s_bar_text, 0, sizeof(s_bar_text)); // every label starts as ""
    for (bar_slot_t slot = 0; slot < BAR_COUNT; slot++) {
        lv_obj_t *label = lv_label_create(s_bar);
        s_bar_labels[slot] = label;
        lv_obj_set_size(label, w[slot], text_h);
        lv_obj_set_pos(label, x[slot], text_y);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        if (slot == BAR_CLOCK) {
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_RIGHT, 0);
        } else if (slot >= BAR_CPU) {
            lv_label_set_recolor(label, true);
        }
        lv_label_set_text(label, "");
    }
    s_bar_timer = lv_timer_create(bar_tick, BAR_REFRESH_MS, NULL);
    bar_refresh();
}

// Status mode returns.
static void bar_delete(void)
{
    if (!s_bar) {
        return;
    }
    lv_timer_delete(s_bar_timer);
    s_bar_timer = NULL;
    lv_obj_delete(s_bar);
    s_bar = NULL;
    memset(s_bar_labels, 0, sizeof(s_bar_labels));
}

void ui_set_stats(const proto_stats_t *stats)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    s_stats = *stats;
    s_stats_at_us = esp_timer_get_time();
    s_have_stats = true;
    bar_refresh();
    lvgl_port_unlock();
}

void ui_show_set(void)
{
    lvgl_port_lock(0);
    s_online = true; // the link applies a set only on a live session
    status_mode_end();
    bar_create();
    bar_refresh();
    // With settings open, the model holds the set and settings_leave builds its page. Settings opened from status mode
    // now returns to grid mode, and the bar replaces its "Settings" title.
    const bool in_settings = s_mode == MODE_SETTINGS;
    if (in_settings) {
        s_settings_from = MODE_GRID;
        settings_bar_returned();
    } else {
        s_mode = MODE_GRID;
    }
    model_lock();
    const uint8_t shown = in_settings ? model_shown() : build_page();
    const uint32_t rev = model_rev();
    const uint8_t pages = model_pages();
    const proto_layout_t *page = model_page(shown);
    const unsigned cols = page->cols, rows = page->rows, buttons = page->n_buttons;
    model_unlock();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "layout rev %u: %u page%s applied, showing page %u (%ux%u, %u buttons)%s", (unsigned)rev, pages,
             pages == 1 ? "" : "s", shown + 1, cols, rows, buttons, in_settings ? ", built when settings closes" : "");
    ui_log_heap(in_settings ? "set applied under settings" : "page built");
    if (!s_heap_logged) {
        s_heap_logged = true;
        ESP_LOGI(TAG, "first layout applied, min free internal heap %u bytes",
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
}

void ui_icons_settled(void)
{
    ui_log_heap("icons settled");
}

void ui_set_icon(const uint8_t id[PROTO_ICON_ID_LEN], const lv_image_dsc_t *dsc)
{
    lvgl_port_lock(0);
    for (size_t i = 0; i < s_n_buttons; i++) {
        if (s_has_icon[i] && memcmp(s_icon_ids[i], id, PROTO_ICON_ID_LEN) == 0) {
            show_icon(i, dsc);
        }
    }
    lvgl_port_unlock();
}

void ui_set_tile(const proto_tile_t *t)
{
    lvgl_port_lock(0);
    size_t i = 0;
    while (i < s_n_buttons && strcmp(s_ids[i], t->btn) != 0) {
        i++;
    }
    if (i < s_n_buttons && s_cell_values[i]) {
        show_tile(i, t);
    } else {
        ESP_LOGD(TAG, "tile for %s stored, its page isn't on screen", t->btn);
    }
    lvgl_port_unlock();
}

void ui_link_down(void)
{
    lvgl_port_lock(0);
    s_online = false;
    s_n_pending = 0;
    if (s_grid) {
        lv_obj_set_style_opa(s_grid, LV_OPA_30, 0);
    }
    bar_refresh();
    lvgl_port_unlock();
}

void ui_show_result(const proto_result_t *r)
{
    lvgl_port_lock(0);
    size_t i = 0;
    while (i < s_n_pending && s_pending[i].seq != r->seq) {
        i++;
    }
    const bool known = i < s_n_pending;
    if (!known) {
        ESP_LOGW(TAG, "result for unknown seq %u ignored", (unsigned)r->seq);
    } else {
        show_result(r, s_pending[i].name);
        s_pending[i] = s_pending[--s_n_pending];
        ESP_LOGI(TAG, "result seq %u %s code %d in %u ms: %s", (unsigned)r->seq, r->ok ? "ok" : "fail", r->code,
                 (unsigned)r->ms, r->summary);
    }
    lvgl_port_unlock();
    if (known) {
        ui_log_heap("result shown"); // the modal's widgets hold the most text right now
    }
}

/* ---- Pairing views, all under the LVGL lock ---- */

static void clear_pair_view(void)
{
    if (s_pair_view) {
        lv_obj_delete(s_pair_view);
        s_pair_view = NULL;
    }
}

// A centered column below the status lines.
static lv_obj_t *new_pair_view(void)
{
    clear_pair_view();
    lv_obj_t *view = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(view);
    lv_obj_set_size(view, DISPLAY_H_RES, DISPLAY_V_RES - PAIR_TOP);
    lv_obj_set_pos(view, 0, PAIR_TOP);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(view, GRID_GAP, 0);
    s_pair_view = view;
    return view;
}

static lv_obj_t *add_text(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, ROW_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, text);
    return label;
}

static void add_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, ROW_W, ROW_H);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_width(label, ROW_W - 32);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
}

static void daemon_tapped(lv_event_t *e)
{
    size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i < s_n_daemons) {
        ESP_LOGI(TAG, "pair with %s", s_daemons[i].name);
        link_pair(&s_daemons[i]);
    }
}

static void cancel_tapped(lv_event_t *e)
{
    link_pair_cancel();
}

static void show_daemon_list(void)
{
    if (s_mode == MODE_SETTINGS) {
        return; // settings_leave shows the list when it brings status mode back
    }
    lv_obj_t *view = new_pair_view();
    if (s_n_daemons == 0) {
        add_text(view, &lv_font_montserrat_32,
                 "No desktop is ready to pair.\nIn the watchdeck app, click Pair a device.");
        return;
    }
    add_text(view, &lv_font_montserrat_32, "Tap your desktop to pair:");
    for (size_t i = 0; i < s_n_daemons; i++) {
        char text[PROTO_NAME_MAX + 24];
        snprintf(text, sizeof(text), "%s  (" IPSTR ")", s_daemons[i].name, IP2STR(&s_daemons[i].ip));
        add_button(view, text, daemon_tapped, (void *)(intptr_t)i);
    }
}

static void show_code(const char *code, const char *daemon)
{
    char name[PROTO_NAME_MAX];
    copy_printable(name, sizeof(name), daemon);
    lv_obj_t *view = new_pair_view();
    add_text(view, &lv_font_montserrat_32, "Pairing code");
    char spaced[8]; // "123 456"
    snprintf(spaced, sizeof(spaced), "%.3s %.3s", code, code + 3);
    add_text(view, &lv_font_montserrat_48, spaced);
    lv_label_set_text_fmt(add_text(view, &lv_font_montserrat_32, ""), "Type it into the watchdeck app on %s.", name);
    add_button(view, "Cancel", cancel_tapped, NULL);
}

static void yes_tapped(lv_event_t *e)
{
    link_pair_confirm();
}

static void show_confirm(const char *device, const char *daemon)
{
    char name[PROTO_NAME_MAX];
    copy_printable(name, sizeof(name), daemon);
    lv_obj_t *view = new_pair_view();
    lv_label_set_text_fmt(add_text(view, &lv_font_montserrat_32, ""), "Does the watchdeck app on %s say \"Paired %s\"?",
                          name, device);
    add_button(view, "Yes", yes_tapped, NULL);
    add_button(view, "No", cancel_tapped, NULL);
    add_text(view, &lv_font_montserrat_32,
             "Nothing is saved here until you tap Yes. If you tap No and the desktop lists this device, click "
             "Forget there.");
}

void ui_set_paired(bool paired)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    s_paired = paired;
    s_attempt_shown = false;
    show_open_settings();
    if (paired) {
        clear_pair_view();
        if (s_forget) {
            lv_obj_set_hidden(s_forget, false);
        }
    } else {
        // Synchronously, since the link frees every icon once this returns.
        settings_delete();
        s_mode = MODE_STATUS;
        if (s_grid) {
            lv_obj_delete(s_grid);
            s_grid = NULL;
        }
        result_hide();
        s_online = false;
        s_n_pending = 0;
        s_n_buttons = 0;
        s_n_daemons = 0;
        s_daemon_name[0] = '\0';
        s_have_stats = false; // the next daemon's clock may differ: show --:-- until its first frame
        memset(&s_stats, 0, sizeof(s_stats));
        bar_delete();
        status_mode_start();
        lv_obj_set_hidden(s_forget, true);
        show_daemon_list();
    }
    lvgl_port_unlock();
}

void ui_show_daemons(const link_daemon_t *daemons, size_t n)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    s_n_daemons = n < LINK_MAX_DAEMONS ? n : LINK_MAX_DAEMONS;
    memcpy(s_daemons, daemons, s_n_daemons * sizeof(daemons[0]));
    for (size_t i = 0; i < s_n_daemons; i++) {
        copy_printable(s_daemons[i].name, sizeof(s_daemons[i].name), s_daemons[i].name);
    }
    if (!s_paired && !s_attempt_shown) {
        show_daemon_list();
    }
    lvgl_port_unlock();
}

// A pairing view arrived while settings is open: the button hides during an attempt, so this is only a late frame. The
// attempt wins, and settings closes into the mode it came from.
static void close_settings_for_attempt(void)
{
    if (s_mode == MODE_SETTINGS) {
        settings_leave();
    }
}

void ui_show_pair_code(const char *code, const char *name)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    s_attempt_shown = code != NULL;
    if (code) {
        close_settings_for_attempt();
        show_code(code, name);
    } else if (!s_paired) {
        show_daemon_list();
    }
    show_open_settings();
    lvgl_port_unlock();
}

void ui_show_pair_confirm(const char *device, const char *name)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    s_attempt_shown = true;
    close_settings_for_attempt();
    show_confirm(device, name);
    show_open_settings();
    lvgl_port_unlock();
}

// Keep the text for status mode and settings, and show it if its status line is on screen. Settings reads it at its
// next refresh.
static void set_text(status_t i, const char *text)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    if (i == STATUS_DAEMON) {
        copy_printable(s_texts[i], STATUS_TEXT_MAX, text);
    } else {
        strlcpy(s_texts[i], text, STATUS_TEXT_MAX);
    }
    show_status(i);
    if (i == STATUS_DAEMON) {
        bar_refresh(); // the bar's left slot shows it while the link is down
    }
    lvgl_port_unlock();
}

void ui_set_wifi_text(const char *text)
{
    set_text(STATUS_WIFI, text);
}

void ui_set_sd_text(const char *text)
{
    set_text(STATUS_SD, text);
}

void ui_set_usb_text(const char *text)
{
    set_text(STATUS_USB, text);
}

void ui_set_touch_text(const char *text)
{
    set_text(STATUS_TOUCH, text);
}

void ui_set_daemon_text(const char *text)
{
    set_text(STATUS_DAEMON, text);
}

void ui_set_daemon_name(const char *name)
{
    if (!s_ready) {
        return;
    }
    lvgl_port_lock(0);
    copy_printable(s_daemon_name, sizeof(s_daemon_name), name);
    lvgl_port_unlock();
}
