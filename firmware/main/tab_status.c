// The Status tab: this device on the left, its connections on the right, and Licenses and Forget pairing along the
// bottom. Everything runs in the LVGL task or under the LVGL lock.

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "icons.h"
#include "link.h"
#include "model.h"
#include "settings.h"
#include "wifi.h"

#define RSSI_FRESH_MS 15000 // an older RSSI shows as "--": the link refreshes it every 5 s while settings is open
#define LINE 31             // a row: Montserrat 24's 27 px line and 4 px
// The columns hold 976 px between the pads. The left one's widest row is "PSRAM 32768 KiB free, min 32768" (410 px) and
// the right one's "connected 192.168.100.200 (watchdeck.local)" (537 px), so 412 and 564 leave a pixel or two. Rows
// that echo a name or an error (SSID, desktop name, "SD: not mounted (ESP_ERR_...)") have no bound and end in dots.
#define LEFT_W 412
#define BTN_W 300
#define BTN_H 56
#define ROW_TEXT_MAX 128

typedef enum {
    ROW_FIRMWARE,
    ROW_IDF,
    ROW_DEVICE,
    ROW_UPTIME,
    ROW_INTERNAL,
    ROW_PSRAM,
    ROW_LVGL,
    ROW_ICONS,
    ROW_LEFT_COUNT,
    ROW_WIFI = ROW_LEFT_COUNT,
    ROW_SSID,
    ROW_RSSI,
    ROW_DAEMON,
    ROW_DESKTOP,
    ROW_SD,
    ROW_USB,
    ROW_TOUCH,
    ROW_LAYOUT,
    ROW_COUNT,
} row_t;

static lv_obj_t *s_rows[ROW_COUNT];
static char s_row_text[ROW_COUNT][ROW_TEXT_MAX]; // what each row was last given: LONG_DOT edits the label's own text

static void set_row(row_t r, const char *text)
{
    ui_set_label(s_rows[r], s_row_text[r], ROW_TEXT_MAX, text);
}

// Fill every row. Takes the model lock after the LVGL lock, and nothing here blocks: the RSSI is the link task's cached
// read, and the device id was read before the first hello.
void tab_status_tick(void)
{
    char text[128];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(text, sizeof(text), "Firmware %s", app->version);
    set_row(ROW_FIRMWARE, text);
    snprintf(text, sizeof(text), "IDF %s", app->idf_ver);
    set_row(ROW_IDF, text);
    snprintf(text, sizeof(text), "Device %s", link_device_id()[0] ? link_device_id() : "--");
    set_row(ROW_DEVICE, text);
    const uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    snprintf(text, sizeof(text), "Uptime %u d %02u:%02u:%02u", (unsigned)(up_s / 86400), (unsigned)(up_s / 3600 % 24),
             (unsigned)(up_s / 60 % 60), (unsigned)(up_s % 60));
    set_row(ROW_UPTIME, text);
    snprintf(text, sizeof(text), "Internal %u KiB free, min %u",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    set_row(ROW_INTERNAL, text);
    snprintf(text, sizeof(text), "PSRAM %u KiB free, min %u",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));
    set_row(ROW_PSRAM, text);
    lv_mem_monitor_t mem;
    lv_mem_monitor(&mem);
    snprintf(text, sizeof(text), "LVGL %u of %u B, max %u", (unsigned)(mem.total_size - mem.free_size),
             (unsigned)mem.total_size, (unsigned)mem.free_biggest_size); // max: the largest free block
    set_row(ROW_LVGL, text);

    size_t icons, icon_bytes;
    model_lock();
    icons_usage(&icons, &icon_bytes);
    const uint32_t rev = model_rev();
    const uint8_t pages = model_pages();
    const uint8_t shown = pages > 0 ? model_shown() : 0;
    model_unlock();
    snprintf(text, sizeof(text), "Icons %u, %u KiB", (unsigned)icons, (unsigned)(icon_bytes / 1024));
    set_row(ROW_ICONS, text);
    snprintf(text, sizeof(text), "Layout rev %u, page %u of %u", (unsigned)rev, shown + 1u, (unsigned)pages);
    set_row(ROW_LAYOUT, text);

    // wifi.c's texts carry no prefix, unlike the SD, USB and Touch ones. A "Wi-Fi: " one would not fit the column
    // beside an IP and the host name, and the texts name Wi-Fi themselves ("connected 192.168.0.5 (watchdeck.local)").
    set_row(ROW_WIFI, ui_status_text(STATUS_WIFI));
    snprintf(text, sizeof(text), "SSID %s", wifi_ssid()[0] ? wifi_ssid() : "--");
    set_row(ROW_SSID, text);
    int dbm;
    uint32_t age_ms;
    if (wifi_rssi(&dbm, &age_ms) && age_ms <= RSSI_FRESH_MS) {
        snprintf(text, sizeof(text), "RSSI %d dBm, %u s ago", dbm, (unsigned)(age_ms / 1000));
    } else {
        strlcpy(text, "RSSI --", sizeof(text));
    }
    set_row(ROW_RSSI, text);
    snprintf(text, sizeof(text), "Daemon: %s", ui_status_text(STATUS_DAEMON));
    set_row(ROW_DAEMON, text);
    snprintf(text, sizeof(text), "Desktop %s", ui_daemon_name()[0] ? ui_daemon_name() : "--");
    set_row(ROW_DESKTOP, text);
    set_row(ROW_SD, ui_status_text(STATUS_SD));
    set_row(ROW_USB, ui_status_text(STATUS_USB));
    set_row(ROW_TOUCH, ui_status_text(STATUS_TOUCH));
}

static lv_obj_t *place_button(lv_obj_t *btn, lv_align_t align, int x)
{
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, align, x, -SETTINGS_PAD);
    return btn;
}

// Montserrat 24 rows in two columns, then Licenses and Forget pairing. Every row fits, so it never scrolls. The text
// style comes from the page and is inherited, so the rows add no style memory of their own.
void tab_status_build(lv_obj_t *parent)
{
    memset(s_row_text, 0, sizeof(s_row_text)); // every label starts as ""
    for (row_t r = 0; r < ROW_COUNT; r++) {
        const bool left = r < ROW_LEFT_COUNT;
        lv_obj_t *label = lv_label_create(parent);
        s_rows[r] = label;
        lv_obj_set_width(label, left ? LEFT_W : SETTINGS_CONTENT_W - 3 * SETTINGS_PAD - LEFT_W);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, SLOT24_H); // one line, ending in dots
        lv_obj_set_pos(label, left ? SETTINGS_PAD : 2 * SETTINGS_PAD + LEFT_W,
                       SETTINGS_PAD + (int)(left ? r : r - ROW_LEFT_COUNT) * LINE);
        lv_label_set_text(label, "");
    }

    place_button(ui_add_licenses_button(parent), LV_ALIGN_BOTTOM_LEFT, SETTINGS_PAD);
    place_button(ui_add_forget(parent), LV_ALIGN_BOTTOM_MID, 0);

    tab_status_tick();
}

void tab_status_leave(void)
{
    memset(s_rows, 0, sizeof(s_rows));
}
