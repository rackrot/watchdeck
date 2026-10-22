#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "display.h"
#include "fonts.h"
#include "link.h"

#define GRID_TOP 204
#define GRID_GAP 8
#define TOAST_MS 2000
#define RESULT_OK_MS 4000 // a result closes itself after this, or at a tap
#define RESULT_FAIL_MS 10000
#define MAX_PENDING 8
#define PENDING_TTL_MS (11 * 60 * 1000) // longest action timeout (10 min) plus margin
#define FORGET_W 300                    // "Forget pairing", right of the touch status line
#define FORGET_H 40
#define FORGET_HOLD_MS 2000
#define ROW_W (DISPLAY_H_RES - 80) // labels and buttons of the pairing views
#define ROW_H 76
#define ICON_PAD 4 // a button with an icon is a column: the icon, a gap, and a label of fixed height
#define ICON_GAP 2
#define ICON_MIN 24       // the label gets two lines unless that leaves the icon less than this
#define TILE_BIG_MIN_H 80 // a tile's value uses the 32 pt font in cells at least this tall, else 24 pt

static const char *TAG = "ui";

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_usb_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_daemon_label;

static lv_obj_t *s_grid;      // NULL until the first layout
static lv_obj_t *s_pair_view; // unpaired: the daemon list, the pairing code, or its question
static lv_obj_t *s_forget;    // shown while paired
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_result;      // a press's result, on the top layer: a backdrop over the grid
static lv_obj_t *s_result_card; // the scrollable panel on the backdrop
static lv_obj_t *s_result_title;
static lv_obj_t *s_result_summary;
static lv_obj_t *s_result_text;
static lv_timer_t *s_result_timer;

// Written and read under the LVGL lock, which also covers the tap callbacks.
static bool s_online;
static bool s_heap_logged;
static uint32_t s_seq;
static struct {
    uint32_t seq;
    uint32_t sent_tick;
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

static lv_obj_t *add_status(lv_palette_t palette, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_palette_lighten(palette, 2), 0);
    lv_obj_set_width(label, DISPLAY_H_RES - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, y);
    lv_label_set_text(label, text);
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

// The label of button `id` in the grid, or `id` itself if the grid no longer has it.
static const char *button_name(const char *id)
{
    for (size_t i = 0; i < s_n_buttons; i++) {
        if (strcmp(s_ids[i], id) == 0 && s_labels[i][0]) {
            return s_labels[i];
        }
    }
    return id;
}

static void show_result(const proto_result_t *r)
{
    const char *verdict = r->ok ? "OK" : "Failed";
    char name[PROTO_ID_MAX]; // button_name falls back to the raw id
    copy_printable(name, sizeof(name), button_name(r->btn));
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

static void create_forget(void)
{
    s_forget = lv_button_create(lv_screen_active());
    lv_obj_set_size(s_forget, FORGET_W, FORGET_H);
    lv_obj_align(s_forget, LV_ALIGN_TOP_RIGHT, -20, 114);
    lv_obj_set_style_bg_color(s_forget, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_add_event_cb(s_forget, forget_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_forget, forget_event, LV_EVENT_RELEASED, NULL);
    lv_obj_t *label = lv_label_create(s_forget);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_label_set_text(label, "Forget pairing");
    lv_obj_center(label);
    lv_obj_set_hidden(s_forget, true);
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

static void cell_event(lv_event_t *e)
{
    const char *id = s_ids[(intptr_t)lv_event_get_user_data(e)];
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
        s_pending[s_n_pending++] = (typeof(s_pending[0])){seq, lv_tick_get()};
        ESP_LOGI(TAG, "press %s seq %u", id, (unsigned)seq);
    }
}

esp_err_t ui_init(lv_display_t *disp)
{
    ESP_RETURN_ON_FALSE(disp, ESP_ERR_INVALID_ARG, TAG, "no display");
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    s_wifi_label = add_status(LV_PALETTE_CYAN, 4, "Wi-Fi: starting");
    s_sd_label = add_status(LV_PALETTE_LIGHT_GREEN, 42, "SD: checking");
    s_usb_label = add_status(LV_PALETTE_AMBER, 80, "USB: starting");
    s_touch_label = add_status(LV_PALETTE_PINK, 118, "Touch: starting");
    lv_obj_set_width(s_touch_label, DISPLAY_H_RES - 60 - FORGET_W); // the touch text is short
    s_daemon_label = add_status(LV_PALETTE_PURPLE, 156, "Daemon: waiting for Wi-Fi");
    create_forget();
    create_toast();
    create_result();
    lvgl_port_unlock();
    return ESP_OK;
}

static void build_grid(const proto_layout_t *l)
{
    if (s_grid) {
        lv_obj_delete(s_grid);
    }
    s_grid = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, DISPLAY_H_RES, DISPLAY_V_RES - GRID_TOP);
    lv_obj_set_pos(s_grid, 0, GRID_TOP);

    const int cell_w = (DISPLAY_H_RES - (l->cols + 1) * GRID_GAP) / l->cols;
    const int cell_h = (DISPLAY_V_RES - GRID_TOP - l->rows * GRID_GAP) / l->rows;
    // The daemon sizes icons to what is left (icons.rs box_px), so both sides use these numbers.
    // Slot line boxes, never a font's own line height, so that no font moves the geometry.
    const int line_h = SLOT32_H;
    const int room = cell_h - 2 * ICON_PAD;
    const int label_lines = room >= 2 * line_h + ICON_GAP + ICON_MIN ? 2 : 1;
    int icon_side = room - label_lines * line_h - ICON_GAP;
    if (icon_side > cell_w - 2 * ICON_PAD) {
        icon_side = cell_w - 2 * ICON_PAD;
    }
    if (icon_side > PROTO_ICON_MAX_SIDE) {
        icon_side = PROTO_ICON_MAX_SIDE;
    }
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
        // A tile is grey until its first value arrives.
        lv_obj_set_style_bg_color(btn, b->tile ? tile_bg(PROTO_TILE_UNKNOWN, b->bg) : lv_color_hex(b->bg), 0);
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
            // 4 or 5 rows there is room for the icon and the value only, so the label goes.
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

void ui_apply_layout(const proto_layout_t *layout)
{
    lvgl_port_lock(0);
    build_grid(layout);
    s_online = true;
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    lvgl_port_unlock();
    ESP_LOGI(TAG, "LVGL heap: %u of %u bytes used now, %u at most, %u%% fragmented",
             (unsigned)(mem.total_size - mem.free_size), (unsigned)mem.total_size, (unsigned)mem.max_used,
             (unsigned)mem.frag_pct);
    ESP_LOGI(TAG, "layout rev %u applied: %ux%u, %u buttons", (unsigned)layout->rev, layout->cols, layout->rows,
             (unsigned)layout->n_buttons);
    if (!s_heap_logged) {
        s_heap_logged = true;
        ESP_LOGI(TAG, "first layout applied, min free internal heap %u bytes",
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
}

void ui_set_icon(const uint8_t id[PROTO_ICON_ID_LEN], const lv_image_dsc_t *dsc)
{
    lvgl_port_lock(0);
    for (size_t i = 0; i < s_n_buttons; i++) {
        if (!s_has_icon[i] || memcmp(s_icon_ids[i], id, PROTO_ICON_ID_LEN) != 0) {
            continue;
        }
        if (!s_cell_icons[i]) {
            s_cell_icons[i] = lv_image_create(s_cells[i]);
            lv_obj_move_to_index(s_cell_icons[i], 0);
        }
        lv_image_set_src(s_cell_icons[i], dsc);
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
    if (i == s_n_buttons || !s_cell_values[i]) {
        ESP_LOGW(TAG, "tile for %s ignored: no such tile button", t->btn);
    } else {
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
    lvgl_port_unlock();
}

void ui_show_result(const proto_result_t *r)
{
    lvgl_port_lock(0);
    size_t i = 0;
    while (i < s_n_pending && s_pending[i].seq != r->seq) {
        i++;
    }
    if (i == s_n_pending) {
        ESP_LOGW(TAG, "result for unknown seq %u ignored", (unsigned)r->seq);
    } else {
        s_pending[i] = s_pending[--s_n_pending];
        show_result(r);
        ESP_LOGI(TAG, "result seq %u %s code %d in %u ms: %s", (unsigned)r->seq, r->ok ? "ok" : "fail", r->code,
                 (unsigned)r->ms, r->summary);
    }
    lvgl_port_unlock();
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
    lv_obj_set_size(view, DISPLAY_H_RES, DISPLAY_V_RES - GRID_TOP);
    lv_obj_set_pos(view, 0, GRID_TOP);
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
    if (!s_forget) {
        return;
    }
    lvgl_port_lock(0);
    s_paired = paired;
    s_attempt_shown = false;
    lv_obj_set_hidden(s_forget, !paired);
    if (paired) {
        clear_pair_view();
    } else {
        if (s_grid) {
            lv_obj_delete(s_grid);
            s_grid = NULL;
        }
        result_hide();
        s_online = false;
        s_n_pending = 0;
        s_n_buttons = 0;
        s_n_daemons = 0;
        show_daemon_list();
    }
    lvgl_port_unlock();
}

void ui_show_daemons(const link_daemon_t *daemons, size_t n)
{
    if (!s_forget) {
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

void ui_show_pair_code(const char *code, const char *name)
{
    if (!s_forget) {
        return;
    }
    lvgl_port_lock(0);
    s_attempt_shown = code != NULL;
    if (code) {
        show_code(code, name);
    } else if (!s_paired) {
        show_daemon_list();
    }
    lvgl_port_unlock();
}

void ui_show_pair_confirm(const char *device, const char *name)
{
    if (!s_forget) {
        return;
    }
    lvgl_port_lock(0);
    s_attempt_shown = true;
    show_confirm(device, name);
    lvgl_port_unlock();
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    lvgl_port_lock(0);
    lv_label_set_text(label, text);
    lvgl_port_unlock();
}

void ui_set_wifi_text(const char *text)
{
    set_text(s_wifi_label, text);
}

void ui_set_sd_text(const char *text)
{
    set_text(s_sd_label, text);
}

void ui_set_usb_text(const char *text)
{
    set_text(s_usb_label, text);
}

void ui_set_touch_text(const char *text)
{
    set_text(s_touch_label, text);
}

void ui_set_daemon_text(const char *text)
{
    if (!s_daemon_label) {
        return;
    }
    char printable[96]; // the longest status link.c builds
    copy_printable(printable, sizeof(printable), text);
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_daemon_label, "Daemon: %s", printable);
    lvgl_port_unlock();
}
