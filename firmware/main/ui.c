#include "ui.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "display.h"
#include "link.h"

#define GRID_TOP 204
#define GRID_GAP 8
#define TOAST_MS 2000
#define TOAST_TEXT_MAX 120
#define MAX_PENDING 8
#define PENDING_TTL_MS (11 * 60 * 1000) // longest action timeout (10 min) plus margin

static const char *TAG = "ui";

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_usb_label;
static lv_obj_t *s_touch_label;
static lv_obj_t *s_daemon_label;

static lv_obj_t *s_grid; // NULL until the first layout
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

// Written and read under the LVGL lock, which also covers the tap callback.
static bool s_online;
static bool s_heap_logged;
static uint32_t s_seq;
static struct {
    uint32_t seq;
    uint32_t sent_tick;
} s_pending[MAX_PENDING];
static size_t s_n_pending;
static char s_ids[PROTO_MAX_BUTTONS][PROTO_ID_MAX];

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
    s_daemon_label = add_status(LV_PALETTE_PURPLE, 156, "Daemon: waiting for Wi-Fi");
    create_toast();
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
    for (size_t i = 0; i < l->n_buttons; i++) {
        const proto_button_t *b = &l->buttons[i];
        strlcpy(s_ids[i], b->id, sizeof(s_ids[i]));

        lv_obj_t *btn = lv_button_create(s_grid);
        lv_obj_set_size(btn, cell_w, cell_h);
        lv_obj_set_pos(btn, GRID_GAP + b->col * (cell_w + GRID_GAP), b->row * (cell_h + GRID_GAP));
        lv_obj_set_style_bg_color(btn, lv_color_hex(b->bg), 0);
        lv_obj_add_event_cb(btn, cell_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_width(label, cell_w - 16);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(b->fg), 0);
        lv_label_set_text(label, b->label);
        lv_obj_center(label);
    }
}

void ui_apply_layout(const proto_layout_t *layout)
{
    lvgl_port_lock(0);
    build_grid(layout);
    s_online = true;
    lvgl_port_unlock();
    ESP_LOGI(TAG, "layout rev %u applied: %ux%u, %u buttons", (unsigned)layout->rev, layout->cols, layout->rows,
             (unsigned)layout->n_buttons);
    if (!s_heap_logged) {
        s_heap_logged = true;
        ESP_LOGI(TAG, "first layout applied, min free internal heap %u bytes",
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
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
        lv_obj_set_style_bg_color(
            s_toast, r->ok ? lv_palette_darken(LV_PALETTE_GREEN, 3) : lv_palette_darken(LV_PALETTE_RED, 3), 0);
        lv_label_set_text_fmt(s_toast, "%s%s%.*s", r->ok ? "OK" : "FAILED", r->text[0] ? ": " : "", TOAST_TEXT_MAX,
                              r->text);
        lv_obj_set_hidden(s_toast, false);
        lv_timer_reset(s_toast_timer);
        lv_timer_resume(s_toast_timer);
        ESP_LOGI(TAG, "result seq %u %s code %d in %u ms", (unsigned)r->seq, r->ok ? "ok" : "fail", r->code,
                 (unsigned)r->ms);
    }
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
    lvgl_port_lock(0);
    lv_label_set_text_fmt(s_daemon_label, "Daemon: %s", text);
    lvgl_port_unlock();
}
