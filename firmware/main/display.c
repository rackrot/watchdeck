#include "display.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define BACKLIGHT_GPIO 31 // active high into the boost EN (10K pull-down)
// LVGL RGB565 is little-endian in memory, which is what the DPI framebuffer expects.
// Set to 1 if red and blue or the colors look wrong.
#define LVGL_SWAP_BYTES 0

static const char *TAG = "display";

static esp_err_t panel_init(esp_lcd_panel_io_handle_t *io, esp_lcd_panel_handle_t *panel)
{
    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_dbi_io_config_t io_cfg = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &io_cfg, io), TAG, "dbi io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 51,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .video_timing =
            {
                .h_size = DISPLAY_H_RES,
                .v_size = DISPLAY_V_RES,
                .hsync_pulse_width = 70,
                .hsync_back_porch = 160,
                .hsync_front_porch = 160,
                .vsync_pulse_width = 10,
                .vsync_back_porch = 23,
                .vsync_front_porch = 12,
            },
    };
    ek79007_vendor_config_t vendor_cfg = {
        .mipi_config =
            {
                .dsi_bus = bus,
                .dpi_config = &dpi_cfg,
                .lane_num = 2,
            },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1, // software reset, as Elecrow does (LCD_RESET is on GPIO41 if needed)
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(*io, &dev_cfg, panel), TAG, "ek79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(*panel), TAG, "dma2d");
    return ESP_OK;
}

void display_backlight(uint8_t percent)
{
    gpio_set_level(BACKLIGHT_GPIO, percent > 0);
}

esp_err_t display_init(lv_display_t **out_disp)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(panel_init(&io, &panel), TAG, "panel init failed");
    ESP_LOGI(TAG, "EK79007 DSI panel initialised");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = DISPLAY_H_RES * 50,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = false,
                .buff_spiram = true,
                .swap_bytes = LVGL_SWAP_BYTES,
            },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = {.flags = {.avoid_tearing = false}};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lvgl_port_add_disp_dsi failed");

    // Backlight on only after the panel is up.
    gpio_config_t bl = {.pin_bit_mask = 1ULL << BACKLIGHT_GPIO, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&bl), TAG, "backlight gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(BACKLIGHT_GPIO, 1), TAG, "backlight on");
    ESP_LOGI(TAG, "display up");
    *out_disp = disp;
    return ESP_OK;
}
