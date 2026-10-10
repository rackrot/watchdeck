#include "ui.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#define SCREEN_W 1024
#define SCREEN_H 600
#define GRID_COLS 5
#define GRID_ROWS 3
#define GRID_TOP 164
#define GRID_GAP 8

static const char *TAG = "ui";

static lv_obj_t *s_wifi_label;
static lv_obj_t *s_sd_label;
static lv_obj_t *s_usb_label;
static lv_obj_t *s_touch_label;

static lv_obj_t *add_status(lv_palette_t palette, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(label, lv_palette_lighten(palette, 2), 0);
    lv_obj_set_width(label, SCREEN_W - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 20, y);
    lv_label_set_text(label, text);
    return label;
}

static void cell_event(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    ESP_LOGI(TAG, "tap r%d c%d (x=%d,y=%d)", idx / GRID_COLS, idx % GRID_COLS, (int)p.x, (int)p.y);
}

// Checkable buttons stay lit after a tap, so a run of taps leaves a map of what registered.
static void add_grid(void)
{
    const int cell_w = (SCREEN_W - (GRID_COLS + 1) * GRID_GAP) / GRID_COLS;
    const int cell_h = (SCREEN_H - GRID_TOP - GRID_ROWS * GRID_GAP) / GRID_ROWS;
    for (int i = 0; i < GRID_COLS * GRID_ROWS; i++) {
        lv_obj_t *btn = lv_button_create(lv_screen_active());
        lv_obj_set_size(btn, cell_w, cell_h);
        lv_obj_set_pos(btn, GRID_GAP + (i % GRID_COLS) * (cell_w + GRID_GAP),
                       GRID_TOP + (i / GRID_COLS) * (cell_h + GRID_GAP));
        lv_obj_set_checkable(btn, true);
        lv_obj_set_style_bg_color(btn, lv_palette_darken(LV_PALETTE_BLUE_GREY, 3), 0);
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_GREEN), LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_palette_main(LV_PALETTE_ORANGE), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, cell_event, LV_EVENT_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_fmt(label, "r%d c%d", i / GRID_COLS, i % GRID_COLS);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_32, 0);
        lv_obj_center(label);
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
    add_grid();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "%dx%d test grid up", GRID_COLS, GRID_ROWS);
    return ESP_OK;
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
