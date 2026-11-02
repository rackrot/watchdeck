#include "battery.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link.h"
#include "svc.h"

#define BOOT_RETRY_MS 20
#define RAW_TEXT_MAX (BATFMT_REGS * 3)
#define READING_TEXT_MAX (96 + RAW_TEXT_MAX)

static const char *TAG = "battery";

static i2c_master_dev_handle_t s_dev;
static esp_timer_handle_t s_timer;
static volatile bool s_queued; // one poll job at a time in svc's queue

// Touched by battery_start before the timer runs, then by the poll job only.
static uint64_t s_period_us;
static unsigned s_fails;           // failed reads in a row
static bool s_timed;               // the first poll's duration has been logged
static bat_charge_t s_last_charge; // of the latest decoded reading, for the change line

// What other tasks read, behind a spinlock held for a copy only.
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static bat_snapshot_t s_snap = {.last_err = ESP_ERR_INVALID_STATE}; // age_us is computed from s_at_us on a copy
static int64_t s_at_us;

static const char *err_text(esp_err_t err)
{
    return err == BATTERY_ERR_IMPLAUSIBLE ? "implausible" : esp_err_to_name(err);
}

static void raw_text(char *out, const uint8_t regs[BATFMT_REGS])
{
    size_t at = 0;
    for (size_t i = 0; i < BATFMT_REGS; i++) {
        at += snprintf(out + at, RAW_TEXT_MAX - at, i ? " %02x" : "%02x", regs[i]);
    }
}

static void reading_text(char *out, size_t size, const bat_reading_t *r, const uint8_t regs[BATFMT_REGS])
{
    char raw[RAW_TEXT_MAX];
    raw_text(raw, regs);
    snprintf(out, size, "%" PRIu32 " mV (pin %" PRIu32 "), %u%%, %s, raw %s", r->mv, r->pin_mv, (unsigned)r->percent,
             batfmt_charge_name(r->charge), raw);
}

// Three short pointer-then-read transfers, one per field. Once the battery is full the STC8 can't keep up with one
// 11-byte burst: the first 4 bytes arrive and the rest read 00, ff or a repeated byte, and the GT911 read after it
// fails (plan C3: 1119 of 2086 polls overnight). Split this way it read 179 of 179 at full, with no touch failures.
static esp_err_t read_regs(uint8_t regs[BATFMT_REGS], bat_reading_t *out)
{
    static const struct {
        uint8_t reg, len;
    } parts[] = {{0x00, 4}, {0x04, 4}, {0x08, 3}};
    memset(regs, 0, BATFMT_REGS);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        const esp_err_t err =
            i2c_master_transmit_receive(s_dev, &parts[i].reg, 1, regs + parts[i].reg, parts[i].len, BATTERY_XFER_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return batfmt_decode(regs, out) ? ESP_OK : BATTERY_ERR_IMPLAUSIBLE;
}

static void store(esp_err_t err, const bat_reading_t *r, int64_t at_us)
{
    taskENTER_CRITICAL(&s_snap_mux);
    s_snap.last_err = err;
    if (r) {
        s_snap.have = true;
        s_snap.reading = *r;
        s_at_us = at_us;
    }
    taskEXIT_CRITICAL(&s_snap_mux);
}

// Only the poll job changes the period, so a job that was queued before a change can't undo it.
static void set_period(uint64_t period_us)
{
    if (period_us == s_period_us) {
        return;
    }
    const esp_err_t err = esp_timer_restart(s_timer, period_us);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "poll period not changed: %s", esp_err_to_name(err));
        return;
    }
    s_period_us = period_us;
}

static void battery_job(void *arg)
{
    s_queued = false;
    uint8_t regs[BATFMT_REGS];
    bat_reading_t r;
    const int64_t start_us = esp_timer_get_time();
    const esp_err_t err = read_regs(regs, &r);
    const int64_t now = esp_timer_get_time();
    store(err, err == ESP_OK ? &r : NULL, now);
    if (!s_timed) {
        s_timed = true;
        ESP_LOGI(TAG, "first poll took %" PRId64 " us (%s)", now - start_us, err_text(err));
    }

    if (err == ESP_OK) {
        char text[READING_TEXT_MAX];
        reading_text(text, sizeof(text), &r, regs);
        if (s_fails >= BATTERY_BACKOFF_FAILS) {
            ESP_LOGI(TAG, "reading again after %u failed reads, polling every %d s: %s", s_fails,
                     BATTERY_POLL_MS / 1000, text);
        }
        s_fails = 0;
        set_period(BATTERY_POLL_MS * 1000ULL);
        if (r.charge != s_last_charge) { // the one condition to make true for a log line every poll (plan C1 to C3)
            ESP_LOGI(TAG, "%s -> %s: %s", batfmt_charge_name(s_last_charge), batfmt_charge_name(r.charge), text);
        }
        s_last_charge = r.charge;
    } else if (++s_fails == BATTERY_BACKOFF_FAILS) {
        ESP_LOGW(TAG, "%u reads failed in a row (%s), polling once a minute", s_fails, err_text(err));
        set_period(BATTERY_BACKOFF_MS * 1000ULL);
    }
    link_battery_poll();
}

static void timer_cb(void *arg)
{
    if (s_queued) {
        return;
    }
    s_queued = true;
    if (svc_post(battery_job, NULL) != ESP_OK) {
        s_queued = false;
    }
}

esp_err_t battery_start(void)
{
    ESP_RETURN_ON_FALSE(!s_dev, ESP_ERR_INVALID_STATE, TAG, "already started");
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(I2C_NUM_0, &bus), TAG, "touch bus");
    // The add is safe beside a touch read: the driver inserts under the bus mutex. The handle is never removed, even
    // when the monitor is absent, since a removal checks the bus before taking that mutex and can race a touch read.
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BATTERY_ADDR,
        .scl_speed_hz = BATTERY_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), TAG, "add 0x%02X", BATTERY_ADDR);
    const esp_timer_create_args_t args = {.callback = timer_cb, .name = "battery"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");

    uint8_t regs[BATFMT_REGS];
    bat_reading_t r;
    esp_err_t err = ESP_FAIL;
    int64_t took_us = 0;
    int tries = 0;
    while (err != ESP_OK && tries < BATTERY_BOOT_TRIES) {
        if (tries++ > 0) {
            vTaskDelay(pdMS_TO_TICKS(BOOT_RETRY_MS));
        }
        const int64_t start_us = esp_timer_get_time();
        err = read_regs(regs, &r);
        took_us = esp_timer_get_time() - start_us;
    }
    store(err, err == ESP_OK ? &r : NULL, esp_timer_get_time());

    if (err != ESP_OK) {
        char raw[RAW_TEXT_MAX] = "";
        if (err == BATTERY_ERR_IMPLAUSIBLE) {
            raw_text(raw, regs);
        }
        ESP_LOGI(TAG, "no monitor at 0x%02X (%s%s%s)", BATTERY_ADDR, err_text(err), raw[0] ? ", raw " : "", raw);
        return ESP_OK;
    }

    char text[READING_TEXT_MAX];
    reading_text(text, sizeof(text), &r, regs);
    ESP_LOGI(TAG, "first reading in %" PRId64 " us (try %d of %d): %s", took_us, tries, BATTERY_BOOT_TRIES, text);
    s_last_charge = r.charge;
    s_period_us = BATTERY_POLL_MS * 1000ULL;
    const esp_err_t start_err = esp_timer_start_periodic(s_timer, s_period_us);
    taskENTER_CRITICAL(&s_snap_mux);
    s_snap.present = start_err == ESP_OK;
    s_snap.have = s_snap.present; // never a reading from a monitor that isn't polled
    taskEXIT_CRITICAL(&s_snap_mux);
    ESP_RETURN_ON_ERROR(start_err, TAG, "timer start");
    return ESP_OK;
}

bool battery_present(void)
{
    taskENTER_CRITICAL(&s_snap_mux);
    const bool present = s_snap.present;
    taskEXIT_CRITICAL(&s_snap_mux);
    return present;
}

void battery_snapshot(bat_snapshot_t *out)
{
    taskENTER_CRITICAL(&s_snap_mux);
    *out = s_snap;
    const int64_t at_us = s_at_us;
    taskEXIT_CRITICAL(&s_snap_mux);
    out->age_us = out->have ? esp_timer_get_time() - at_us : 0;
}
