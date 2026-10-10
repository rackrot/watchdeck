#pragma once

#include "esp_err.h"

typedef void (*wifi_status_cb_t)(const char *text);

/**
 * Start station mode through the ESP-Hosted C6. Credentials come from
 * /sdcard/wifi.txt (line 1 SSID, line 2 password) if present, else Kconfig.
 * With no credentials it does not connect. Progress goes to status_cb.
 * Requires NVS, esp_netif, and the default event loop to be initialised.
 */
esp_err_t wifi_start(wifi_status_cb_t status_cb);
