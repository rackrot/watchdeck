#include "cardformat.h"

#include <stdatomic.h>

#include "cardmkfs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdcard.h"
#include "svc.h"
#include "usb.h"

static const char *TAG = "cardformat";

static _Atomic bool s_queued; // cleared with a release store once s_result is final
static _Atomic bool s_unseen; // s_result is new and cardformat_take_result hasn't returned it
static int64_t s_started_us;
static cardformat_result_t s_result;

static void format_job(void *arg)
{
    cardformat_result_t r = {.step = "starting"};
    sdcard_info_t info;
    sdcard_info(&info);
    cardmkfs_layout_t layout;
    if (cardmkfs_layout((uint32_t)(info.bytes / CARDMKFS_SECTOR), &layout)) {
        r.cluster_bytes = layout.cluster_bytes;
    }
    if (usb_share_busy() || sdcard_owner() == SDCARD_SHARED) {
        r.err = ESP_ERR_INVALID_STATE; // a share went ahead of this job
    } else {
        ESP_LOGW(TAG, "formatting the card");
        r.err = sdcard_format(&r.reached, &r.step);
        if (r.err == ESP_OK) {
            r.wifi = wifi_save_current();
            ESP_LOGI(TAG, "wifi.txt after the format: %s",
                     r.wifi == WIFI_SAVE_OK     ? "saved"
                     : r.wifi == WIFI_SAVE_NONE ? "no network set, not written"
                                                : "not saved");
        }
    }
    r.seconds = (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
    s_result = r;
    atomic_store(&s_unseen, true);
    atomic_store_explicit(&s_queued, false, memory_order_release);
}

esp_err_t cardformat_start(void)
{
    if (!sdcard_card()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (sdcard_owner() == SDCARD_SHARED || usb_share_busy() || cardformat_busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_started_us = esp_timer_get_time();
    atomic_store(&s_unseen, false); // an older result nobody saw is replaced
    atomic_store_explicit(&s_queued, true, memory_order_release);
    const esp_err_t err = svc_post(format_job, NULL);
    if (err != ESP_OK) {
        atomic_store_explicit(&s_queued, false, memory_order_release);
    }
    return err;
}

bool cardformat_busy(void)
{
    return atomic_load_explicit(&s_queued, memory_order_acquire);
}

uint32_t cardformat_seconds(void)
{
    return (uint32_t)((esp_timer_get_time() - s_started_us) / 1000000);
}

bool cardformat_take_result(cardformat_result_t *out)
{
    if (cardformat_busy() || !atomic_exchange(&s_unseen, false)) {
        return false;
    }
    *out = s_result;
    return true;
}
