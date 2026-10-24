#include "usb.h"

#include <stdio.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdcard.h"
#include "sdmmc_cmd.h"
#include "svc.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

static const char *TAG = "usb";

static usb_status_cb_t s_status_cb;
static bool s_ready;             // TinyUSB and its MSC driver are installed, with esp_tinyusb's mounting off
static volatile bool s_attached; // a host has the device configured
static volatile bool s_share_queued;
static esp_err_t s_error; // the last start or share failure, shown until a share succeeds
// Kept while shared: Stop sharing restarts the board, so they're never released.
static esp_blockdev_handle_t s_bdev;
static tinyusb_msc_storage_handle_t s_storage;

void usb_refresh_status(void)
{
    char text[64];
    if (s_error != ESP_OK) {
        snprintf(text, sizeof(text), "USB: error (%s)", esp_err_to_name(s_error));
    } else {
        switch (sdcard_owner()) {
        case SDCARD_DEVICE:
            snprintf(text, sizeof(text), "USB: card on the device");
            break;
        case SDCARD_SHARED:
            snprintf(text, sizeof(text), "USB: sharing the card (%s)",
                     s_attached ? "connected to a computer" : "no computer");
            break;
        default:
            snprintf(text, sizeof(text), "USB: no card");
            break;
        }
    }
    ESP_LOGI(TAG, "%s", text);
    if (s_status_cb) {
        s_status_cb(text);
    }
}

static void on_usb_event(tinyusb_event_t *event, void *arg)
{
    // shortcut: no VBUS sensing on this board, so an unplug may never show as a detach and the line keeps saying
    // "connected"; enable CONFIG_TINYUSB_SUSPEND_CALLBACK and count a suspend as unplugged if batch A shows that.
    if (event->id == TINYUSB_EVENT_ATTACHED || event->id == TINYUSB_EVENT_DETACHED) {
        s_attached = event->id == TINYUSB_EVENT_ATTACHED;
        usb_refresh_status();
    }
}

// With esp_tinyusb's mounting off and the storage only ever mapped to USB, nothing should raise these.
static void on_msc_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    ESP_LOGW(TAG, "MSC event %d (mount point %d)", event->id, event->mount_point);
}

esp_err_t usb_start(usb_status_cb_t status_cb)
{
    s_status_cb = status_cb;
    // Mounting off: a host's eject (Start Stop Unit) would otherwise mount the raw GPT disk to the app, find no
    // file system, and take esp_tinyusb's format path.
    const tinyusb_msc_driver_config_t msc_cfg = {.user_flags = {.auto_mount_off = 1}, .callback = on_msc_event};
    esp_err_t err = tinyusb_msc_install_driver(&msc_cfg);
    const bool msc_installed = err == ESP_OK;
    if (err == ESP_OK) {
        const tinyusb_config_t usb_cfg = TINYUSB_CONFIG_HIGH_SPEED(on_usb_event, NULL);
        err = tinyusb_driver_install(&usb_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB start failed: %s", esp_err_to_name(err));
        if (msc_installed) {
            tinyusb_msc_uninstall_driver();
        }
        s_error = err;
    } else {
        s_ready = true;
    }
    usb_refresh_status();
    return err;
}

// Maps the raw card to USB. The card is already unmounted. Nothing is mapped when this fails.
static esp_err_t map_card(void)
{
    esp_err_t err = sdmmc_get_blockdev(sdcard_card(), &s_bdev);
    if (err == ESP_OK) {
        const tinyusb_msc_storage_config_t cfg = {
            .medium.blockdev = s_bdev,
            .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
            .fat_fs = {.do_not_format = true},
        };
        err = tinyusb_msc_new_storage_blockdev(&cfg, &s_storage);
    }
    if (err != ESP_OK && s_bdev) {
        s_bdev->ops->release(s_bdev);
        s_bdev = NULL;
    }
    return err;
}

static void share_job(void *arg)
{
    s_share_queued = false;
    if (sdcard_owner() != SDCARD_DEVICE) {
        ESP_LOGW(TAG, "share: the card isn't the device's, nothing to do");
        return;
    }
    esp_err_t err = sdcard_release_fs(); // waits for a cardfs call in progress, then unmounts
    if (err == ESP_OK) {
        err = map_card();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "share: mapping the card to USB failed: %s; taking it back", esp_err_to_name(err));
            sdcard_remount(); // nothing reached USB, so the device can mount it again
        }
    }
    s_error = err;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "card shared over USB; writes are off until Stop sharing restarts the board");
        sdcard_set_shared(); // the owner callback refreshes the USB line
    } else {
        usb_refresh_status();
    }
}

esp_err_t usb_share(void)
{
    // Without the driver usb_start installed, the storage call would install esp_tinyusb's default one, with its
    // mounting on.
    if (!s_ready || sdcard_owner() != SDCARD_DEVICE || s_share_queued) {
        return ESP_ERR_INVALID_STATE;
    }
    s_share_queued = true;
    const esp_err_t err = svc_post(share_job, NULL);
    if (err != ESP_OK) {
        s_share_queued = false;
    }
    return err;
}

void usb_stop_sharing(void)
{
    ESP_LOGW(TAG, "stop sharing: restarting to take the card back");
    esp_restart();
}
