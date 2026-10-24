#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "wifistate.h"

typedef void (*wifi_status_cb_t)(const char *text);

#define WIFI_SSID_LEN (WIFISCAN_SSID_MAX + 1)

/**
 * Bring Wi-Fi up through the ESP-Hosted C6, with or without credentials: the netif, mDNS, the driver, the timers and
 * the event handlers, and station mode started with no config. Progress goes to status_cb, which may take the LVGL
 * lock, so it is never called with a Wi-Fi lock held. Requires NVS, esp_netif, the default event loop and svc_start.
 */
esp_err_t wifi_init(wifi_status_cb_t status_cb);

/**
 * Join the network saved for boot: /sdcard/wifi.txt (line 1 SSID, line 2 password) if it reads, else Kconfig. With
 * none, it connects to nothing and says so. Run once, after wifi_init.
 */
esp_err_t wifi_apply_saved(void);

/** Copy the SSID the board is on, or retries, into out ("" before it has one). Safe from any task. */
void wifi_current_ssid(char out[WIFI_SSID_LEN]);

/** Where those credentials came from: "sd" (wifi.txt), "config" (Kconfig), "typed" (entered on the screen and not
 * saved) or "none". */
const char *wifi_source(void);

/** What the last look at wifi.txt on the card found. WIFI_FILE_UNKNOWN: not looked yet. */
typedef enum {
    WIFI_FILE_UNKNOWN,
    WIFI_FILE_FOUND,
    WIFI_FILE_ABSENT,
    WIFI_FILE_SHARED, // the card is shared over USB, so nothing is known
    WIFI_FILE_NO_CARD,
} wifi_file_t;
wifi_file_t wifi_file_state(void);

/** Looks for wifi.txt on the svc task, which sets wifi_file_state() when it runs. Never blocks. */
void wifi_probe_file(void);

/** The station's IPv4 address as text, false when it has none. */
bool wifi_ip(char *out, size_t out_size);

/**
 * Whether the UI wants the RSSI. While on, a svc job refreshes the cached value every few seconds, and not during a
 * scan or while a trial runs. Never blocks.
 */
void wifi_watch_rssi(bool on);

/** The cached RSSI and how long ago it was read. False before the first successful refresh. Safe from any task. */
bool wifi_rssi(int *dbm, uint32_t *age_ms);

/*
 * The jobs the Wi-Fi tab starts. Each returns at once, never blocks, runs on svc, and one runs at a time. The tab
 * polls wifi_job_get; a job that ends leaves its result there until the next one starts.
 *
 *   wifi_scan:  scan and keep up to WIFISCAN_MAX networks, strongest first (wifi_scan_results).
 *   wifi_try:   the trial connect. Stops the retry timer, joins the network, and on an IP for that SSID makes it the
 *               current network and writes wifi.txt. On a failure the old network comes back and nothing is written.
 *   wifi_save:  write wifi.txt without joining anything; the board reads it at the next boot.
 *
 * ESP_ERR_INVALID_ARG: wifitxt refuses the SSID or password. ESP_ERR_INVALID_STATE: a job runs, wifi_init hasn't
 * run, or (wifi_scan) a trial or restore is under way. ESP_ERR_NO_MEM: svc's queue is full.
 */
esp_err_t wifi_scan(void);
esp_err_t wifi_try(const char *ssid, const char *pass);
esp_err_t wifi_save(const char *ssid, const char *pass);

/** Copy the scan list; returns how many entries. */
size_t wifi_scan_results(wifiscan_entry_t *out, size_t max);

typedef enum {
    WIFI_JOB_NONE,
    WIFI_JOB_SCAN,
    WIFI_JOB_TRIAL,
    WIFI_JOB_SAVE,
} wifi_job_kind_t;

// What writing wifi.txt came to. WIFI_SAVE_NONE: no write was tried.
typedef enum {
    WIFI_SAVE_NONE,
    WIFI_SAVE_OK,
    WIFI_SAVE_NO_CARD,   // no card was mounted
    WIFI_SAVE_SHARED,    // the card is shared over USB
    WIFI_SAVE_READ_ONLY, // mounted, but cardfs can't write it (not FAT32)
    WIFI_SAVE_TAINTED,   // an earlier card access failed; the card has been remounted
    WIFI_SAVE_DIR_FULL,  // the card's root folder has no room for the entry
    WIFI_SAVE_BUSY,      // svc's queue was full
    WIFI_SAVE_ERROR,     // another card error
} wifi_save_t;

typedef struct {
    wifi_job_kind_t kind; // the latest job started
    bool running;
    bool joined; // a trial joined the network and wifi.txt is being written
    bool ok;     // when !running: a scan finished, a trial joined, or a save wrote
    wifi_fail_t fail;
    wifi_save_t save;
    esp_err_t err;   // a failed scan's error
    uint32_t serial; // counts jobs started
} wifi_job_t;

void wifi_job_get(wifi_job_t *out);
