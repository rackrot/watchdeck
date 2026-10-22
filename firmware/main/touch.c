#include "touch.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_SDA_GPIO 45
#define TOUCH_SCL_GPIO 46
#define TOUCH_RST_GPIO 40 // active low
#define TOUCH_INT_GPIO 42 // no pull-up fitted, so the GT911 is polled
#define TOUCH_I2C_HZ 400000
#define TOUCH_READ_MS 10 // a GT911 read is about 3 short I2C transfers at 400 kHz, roughly 1 to 2 ms
#define TOUCH_H_RES 1024
#define TOUCH_V_RES 600

// Calibration knobs. Applied by the esp_lcd_touch driver to the raw GT911 coordinates.
// Flip these if a tap lands mirrored or transposed relative to the button highlighted.
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 0
#define TOUCH_MIRROR_Y 0

static const char *TAG = "touch";

// The GT911 latches its I2C address from the INT level at the rising edge of RST:
// INT low selects 0x5D, INT high selects 0x14. Afterwards INT is released as an input.
static esp_err_t reset_with_address(uint8_t addr)
{
    const gpio_config_t out = {
        .pin_bit_mask = BIT64(TOUCH_RST_GPIO) | BIT64(TOUCH_INT_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "rst/int gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST_GPIO, 0), TAG, "rst low");
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_INT_GPIO, addr == ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP), TAG,
                        "int level");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RST_GPIO, 1), TAG, "rst high");
    vTaskDelay(pdMS_TO_TICKS(10));

    const gpio_config_t in = {.pin_bit_mask = BIT64(TOUCH_INT_GPIO), .mode = GPIO_MODE_INPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "int gpio");
    vTaskDelay(pdMS_TO_TICKS(60));
    return ESP_OK;
}

esp_err_t touch_init(lv_display_t *disp)
{
    esp_err_t ret = ESP_OK;
    i2c_master_bus_handle_t bus = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_touch_handle_t tp = NULL;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, // 2K external pull-ups on LDO 4
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "i2c bus");

    const uint8_t addrs[] = {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP};
    uint8_t addr = 0;
    for (size_t i = 0; i < sizeof(addrs) && !addr; i++) {
        ESP_GOTO_ON_ERROR(reset_with_address(addrs[i]), err, TAG, "reset");
        if (i2c_master_probe(bus, addrs[i], 100) == ESP_OK) {
            addr = addrs[i];
        } else {
            ESP_LOGW(TAG, "no ACK at 0x%02x", addrs[i]);
        }
    }
    ESP_GOTO_ON_FALSE(addr, ESP_ERR_NOT_FOUND, err, TAG, "GT911 not found at 0x5D or 0x14");

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.dev_addr = addr;
    io_cfg.scl_speed_hz = TOUCH_I2C_HZ;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), err, TAG, "panel io");

    // RST and INT are NC here: the address was latched above, and an INT pin makes
    // esp_lvgl_port expect edge interrupts, which a floating INT can't deliver.
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = TOUCH_H_RES,
        .y_max = TOUCH_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {.swap_xy = TOUCH_SWAP_XY, .mirror_x = TOUCH_MIRROR_X, .mirror_y = TOUCH_MIRROR_Y},
    };
    ESP_GOTO_ON_ERROR(esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, &tp), err, TAG, "gt911");

    const lvgl_port_touch_cfg_t lv_cfg = {.disp = disp, .handle = tp};
    lv_indev_t *indev = lvgl_port_add_touch(&lv_cfg);
    ESP_GOTO_ON_FALSE(indev, ESP_FAIL, err, TAG, "lvgl_port_add_touch failed");
    // The INT pin is not usable, so LVGL polls. Its default period is LV_DEF_REFR_PERIOD (33 ms), too slow to see a
    // fast flick move between samples. With the 100 Hz FreeRTOS tick the esp_lvgl_port loop can't hit 10 ms exactly;
    // the board measured about 10 to 15 ms per read (S8). Smaller per-read steps also make LVGL's gesture check (3 px
    // per read) miss more flicks, which ui.c's drag fallback turns into page turns.
    lvgl_port_lock(0);
    lv_timer_set_period(lv_indev_get_read_timer(indev), TOUCH_READ_MS);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "GT911 at 0x%02x, %dx%d, swap_xy=%d mirror_x=%d mirror_y=%d, registered with LVGL", addr, TOUCH_H_RES,
             TOUCH_V_RES, TOUCH_SWAP_XY, TOUCH_MIRROR_X, TOUCH_MIRROR_Y);
    return ESP_OK;

err:
    if (tp) {
        esp_lcd_touch_del(tp);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    i2c_del_master_bus(bus);
    return ret;
}
