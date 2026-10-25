#pragma once

// Format card (Settings > Storage): a svc job that erases the TF card and makes it one FAT32 partition
// (sdcard_format, cardmkfs.h), then saves the Wi-Fi settings in use back to wifi.txt (wifi_save_current). No backup is
// kept. The job blocks svc for the whole format, tens of seconds on a large card; other card jobs wait behind it.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifi.h"

typedef struct {
    esp_err_t err;          // ESP_OK: formatted and mounted
    const char *step;       // what it was doing when it failed (a literal), NULL on success
    wifi_save_t wifi;       // after a success: the wifi.txt write; WIFI_SAVE_NONE when no network is set
    uint32_t cluster_bytes; // the cluster size it chose
    uint32_t seconds;       // how long the job took
} cardformat_result_t;

/**
 * Queue the format. ESP_ERR_NOT_FOUND: no card answered at boot. ESP_ERR_INVALID_STATE: the card is shared over USB,
 * a share is queued or running, or a format is. ESP_ERR_NO_MEM: svc's queue is full.
 */
esp_err_t cardformat_start(void);

/** Whether a format is queued or running. Once it ends, cardformat_result() is final. Safe from any task. */
bool cardformat_busy(void);

/** Seconds since the format was queued, while it's busy. */
uint32_t cardformat_seconds(void);

/** The last format's result; read it once cardformat_busy() has gone false after a start. */
void cardformat_result(cardformat_result_t *out);
