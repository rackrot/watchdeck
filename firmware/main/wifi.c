#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"
#include "diag.h"
#include "logcap.h"
#include "sdcard.h"

#define RECONNECT_DELAY_US (3 * 1000 * 1000)
#define HOSTNAME "watchdeck"
#define WIFI_FILE SDCARD_MOUNT_POINT "/wifi.txt"

static const char *TAG = "wifi";

static wifi_status_cb_t s_status_cb;
static esp_timer_handle_t s_reconnect_timer;
static char s_ssid[33];
// The last RSSI read and when, written by the link task and read by the LVGL task.
static portMUX_TYPE s_rssi_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_rssi_valid;
static int s_rssi_dbm;
static int64_t s_rssi_at_us;

static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void report(const char *fmt, ...)
{
    char text[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", text);
    if (s_status_cb) {
        s_status_cb(text);
    }
}

#define SSID_MAX 32
#define PASS_MAX 64

// Read one line, strip the line ending, and enforce max_len. Returns false on EOF or an
// over-long line (buf must hold max_len plus CR, LF, and NUL). Only the length is logged.
static bool read_line(FILE *f, char *buf, size_t buf_len, size_t max_len, const char *what)
{
    if (!fgets(buf, buf_len, f)) {
        return false;
    }
    bool has_newline = strchr(buf, '\n') != NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    if ((!has_newline && !feof(f)) || strlen(buf) > max_len) {
        ESP_LOGE(TAG, "%s in %s is longer than %u chars", what, WIFI_FILE, (unsigned)max_len);
        return false;
    }
    return true;
}

// Read SSID and password from the card. Returns false when the file is absent or invalid.
static bool read_card_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    FILE *f = fopen(WIFI_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "no %s, using Kconfig credentials", WIFI_FILE);
        return false;
    }
    bool ok = read_line(f, ssid, ssid_len, SSID_MAX, "SSID");
    if (!ok && feof(f)) {
        ESP_LOGW(TAG, "%s is empty", WIFI_FILE);
    }
    if (ok) {
        int c = fgetc(f);
        if (c == EOF) {
            pass[0] = '\0'; // open network: password line missing
        } else {
            ungetc(c, f);
            ok = read_line(f, pass, pass_len, PASS_MAX, "password");
        }
    }
    fclose(f);
    return ok;
}

// Set the DHCP hostname and answer as HOSTNAME.local. Failures are logged and boot continues.
static void start_mdns(esp_netif_t *sta)
{
    // mdns binds to the netif registered as "WIFI_STA_DEF" (its predefined STA interface).
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != sta) {
        ESP_LOGE(TAG, "STA netif is not WIFI_STA_DEF, mDNS will not bind to it");
    }
    esp_err_t err = esp_netif_set_hostname(sta, HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_hostname: %s", esp_err_to_name(err));
    }
    err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(HOSTNAME);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS setup: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mDNS hostname %s.local", HOSTNAME);
    }
}

static void reconnect_cb(void *arg)
{
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        reconnect_cb(NULL);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        diag_count(DIAG_WIFI_DROP, wifi_reason_name(ev->reason));
        report("connecting to %s (retry, reason %d)", s_ssid, ev->reason);
        esp_err_t err = esp_timer_start_once(s_reconnect_timer, RECONNECT_DELAY_US);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE: retry already armed
            ESP_LOGE(TAG, "retry timer: %s", esp_err_to_name(err));
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
    report("connected " IPSTR " (" HOSTNAME ".local)", IP2STR(&ev->ip_info.ip));
}

esp_err_t wifi_start(wifi_status_cb_t status_cb)
{
    s_status_cb = status_cb;

    char ssid[SSID_MAX + 3] = "";
    char pass[PASS_MAX + 3] = "";
    const char *source = "sd";
    if (!read_card_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        source = "config";
        if (strlen(CONFIG_WATCHDECK_WIFI_SSID) > SSID_MAX || strlen(CONFIG_WATCHDECK_WIFI_PASSWORD) > PASS_MAX) {
            ESP_LOGE(TAG, "Kconfig SSID or password is too long");
            report("invalid Wi-Fi credentials");
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(ssid, CONFIG_WATCHDECK_WIFI_SSID);
        strcpy(pass, CONFIG_WATCHDECK_WIFI_PASSWORD);
    }
    // The card goes to the USB host once wifi.txt has been read.
    sdcard_release_fs();

    // An empty SSID would join any open AP, so refuse.
    if (ssid[0] == '\0') {
        report("no Wi-Fi credentials");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    logcap_set_password(pass); // so the log ring redacts it

    report("connecting to %s (source: %s)", s_ssid, source);

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }
    start_mdns(sta);
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");

    const esp_timer_create_args_t timer_args = {.callback = reconnect_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_reconnect_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL), TAG, "ip evt");

    wifi_config_t cfg = {0};
    // Lengths are validated above; a full-length field needs no NUL.
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));
    if (pass[0] != '\0') {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // don't join an open AP with the same SSID
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    return ESP_OK;
}

const char *wifi_ssid(void)
{
    return s_ssid;
}

esp_err_t wifi_refresh_rssi(void)
{
    int dbm;
    esp_err_t err = esp_wifi_sta_get_rssi(&dbm);
    if (err != ESP_OK) {
        return err;
    }
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_rssi_mux);
    s_rssi_valid = true;
    s_rssi_dbm = dbm;
    s_rssi_at_us = now;
    taskEXIT_CRITICAL(&s_rssi_mux);
    return ESP_OK;
}

bool wifi_rssi(int *dbm, uint32_t *age_ms)
{
    taskENTER_CRITICAL(&s_rssi_mux);
    const bool valid = s_rssi_valid;
    *dbm = s_rssi_dbm;
    const int64_t at_us = s_rssi_at_us;
    taskEXIT_CRITICAL(&s_rssi_mux);
    *age_ms = (uint32_t)((esp_timer_get_time() - at_us) / 1000);
    return valid;
}
