#include "usb.h"

#include "esp_check.h"
#include <stdio.h>

#include "esp_log.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usb";

static usb_status_cb_t s_status_cb;

static void report(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
    if (s_status_cb) {
        s_status_cb(text);
    }
}

static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        report("USB: connected to host");
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        report("USB: disconnected");
    }
}

esp_err_t usb_msc_start(usb_status_cb_t status_cb)
{
    s_status_cb = status_cb;

    sdmmc_card_t *card = sdcard_card();
    if (!card) {
        report("USB: no card");
        return ESP_ERR_INVALID_STATE;
    }

    // Never expose a card that FatFs still has mounted.
    sdcard_release_fs();

    // Raw block device over the card. The storage stays owned by the USB host and
    // auto-mount is off, so esp_tinyusb never mounts, formats, or touches FAT itself.
    esp_blockdev_handle_t bdev = NULL;
    tinyusb_msc_storage_handle_t storage = NULL;
    bool driver_installed = false;
    esp_err_t err = sdmmc_get_blockdev(card, &bdev);
    if (err == ESP_OK) {
        const tinyusb_msc_driver_config_t drv_cfg = {.user_flags = {.auto_mount_off = 1}};
        err = tinyusb_msc_install_driver(&drv_cfg);
        driver_installed = err == ESP_OK;
    }
    if (err == ESP_OK) {
        const tinyusb_msc_storage_config_t storage_cfg = {
            .medium.blockdev = bdev,
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        };
        err = tinyusb_msc_new_storage_blockdev(&storage_cfg, &storage);
    }
    if (err == ESP_OK) {
        const tinyusb_config_t usb_cfg = TINYUSB_CONFIG_HIGH_SPEED(on_usb_event, NULL);
        err = tinyusb_driver_install(&usb_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB MSC start failed: %s", esp_err_to_name(err));
        if (storage) {
            tinyusb_msc_delete_storage(storage);
        }
        if (driver_installed) {
            tinyusb_msc_uninstall_driver();
        }
        if (bdev) {
            bdev->ops->release(bdev);
        }
        char text[64];
        snprintf(text, sizeof(text), "USB: error (%s)", esp_err_to_name(err));
        report(text);
        return err;
    }
    report("USB: drive ready");
    return ESP_OK;
}
