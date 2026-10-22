#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*wifi_status_cb_t)(const char *text);

/**
 * Start station mode through the ESP-Hosted C6. Credentials come from
 * /sdcard/wifi.txt (line 1 SSID, line 2 password) if present, else Kconfig.
 * With no credentials it does not connect. Progress goes to status_cb.
 * Requires NVS, esp_netif, and the default event loop to be initialised.
 */
esp_err_t wifi_start(wifi_status_cb_t status_cb);

/** The SSID wifi_start joins, or "" before it has one. Set once at boot, so safe to read from any task. */
const char *wifi_ssid(void);

/**
 * Ask the C6 for the station's RSSI and cache it with the time. An RPC over SDIO that can take a while, so never call
 * it on the LVGL task. Fails while not associated, leaving the cached value as it was.
 */
esp_err_t wifi_refresh_rssi(void);

/** The cached RSSI and how long ago it was read. False before the first successful refresh. Safe from any task. */
bool wifi_rssi(int *dbm, uint32_t *age_ms);
