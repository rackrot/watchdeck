#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "watchdeck";

void app_main(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "watchdeck starting: chip rev v%d.%d, PSRAM %u bytes", info.revision / 100, info.revision % 100,
             (unsigned)psram);
}
