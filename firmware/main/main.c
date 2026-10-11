#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "display.h"
#include "link.h"
#include "sdcard.h"
#include "touch.h"
#include "ui.h"
#include "usb.h"
#include "wifi.h"

static const char *TAG = "watchdeck";

// LDO 3 feeds the MIPI DSI PHY; LDO 4 feeds VDDPST_5 (I2C, touch, LCD reset, level shifters).
static esp_err_t power_init(void)
{
    const struct {
        int chan;
        int mv;
    } rails[] = {{3, 2500}, {4, 3300}};
    for (size_t i = 0; i < sizeof(rails) / sizeof(rails[0]); i++) {
        esp_ldo_channel_handle_t handle;
        esp_ldo_channel_config_t cfg = {.chan_id = rails[i].chan, .voltage_mv = rails[i].mv};
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&cfg, &handle), TAG, "LDO %d", rails[i].chan);
        ESP_LOGI(TAG, "LDO chan %d acquired at %d mV", rails[i].chan, rails[i].mv);
    }
    return ESP_OK;
}

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "watchdeck starting: chip rev v%d.%d, PSRAM %u bytes", info.revision / 100, info.revision % 100,
             (unsigned)psram);

    ESP_ERROR_CHECK(power_init());

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // SD and screen failures must not stop the rest, so report and carry on.
    char sd_text[64];
    sdcard_mount(sd_text, sizeof(sd_text));

    lv_display_t *disp = NULL;
    esp_err_t err = display_init(&disp);
    if (err == ESP_OK) {
        err = ui_init(disp);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display/ui init: %s", esp_err_to_name(err));
    }
    ui_set_sd_text(sd_text);

    // Touch needs the LVGL display, and its failure must not stop boot.
    err = disp ? touch_init(disp) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_init: %s", esp_err_to_name(err));
    }
    ui_set_touch_text(err == ESP_OK ? "Touch: ready" : "Touch: FAILED");

    err = wifi_start(ui_set_wifi_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
    }

    // The link waits for an IP itself, so it starts even if wifi_start failed.
    const link_cbs_t link_cbs = {
        .on_status = ui_set_daemon_text,
        .on_layout = ui_apply_layout,
        .on_down = ui_link_down,
        .on_result = ui_show_result,
        .on_paired = ui_set_paired,
        .on_daemons = ui_show_daemons,
        .on_pair_code = ui_show_pair_code,
        .on_pair_confirm = ui_show_pair_confirm,
    };
    err = link_start(&link_cbs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "link_start: %s", esp_err_to_name(err));
    }

    // wifi_start releases the filesystem after reading wifi.txt, even when it fails.
    err = usb_msc_start(ui_set_usb_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_msc_start: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "boot done, min free internal heap %u bytes",
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
