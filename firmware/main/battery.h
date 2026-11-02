#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "batfmt.h"
#include "esp_err.h"

/*
 * The battery monitor: the STC8 at 0x2F on the touch bus (I2C0), read on the svc task every BATTERY_POLL_MS. Only
 * 0x2F is touched, and nothing is written but the register pointer. The decoding is in batfmt.c, and the register map
 * in site/src/device-spec.md "Battery monitoring".
 */

#define BATTERY_ADDR 0x2F
#define BATTERY_I2C_HZ 100000
#define BATTERY_XFER_MS 50       // per transfer; the IDF applies it to the bus mutex wait and the transfer separately
#define BATTERY_BOOT_TRIES 3     // reads at boot before the monitor counts as absent
#define BATTERY_POLL_MS 5000     // svc job period while reads succeed
#define BATTERY_BACKOFF_MS 60000 // svc job period after BATTERY_BACKOFF_FAILS failed reads in a row
#define BATTERY_BACKOFF_FAILS 3  // failed reads in a row that move the poll to BATTERY_BACKOFF_MS
#define BATTERY_ERR_IMPLAUSIBLE ESP_ERR_INVALID_CRC // the bytes arrived but batfmt_decode refused them

typedef struct {
    bool present;          // the STC8 answered with a plausible reading at boot; fixed after battery_start
    bool have;             // a reading has been decoded since boot (pass it and age_us to batfmt_fresh)
    bat_reading_t reading; // the latest decoded reading, valid when `have`
    int64_t age_us;        // since `reading` was taken
    esp_err_t last_err;    // the latest read: ESP_OK, an I2C error, BATTERY_ERR_IMPLAUSIBLE; INVALID_STATE before start
} bat_snapshot_t;

/**
 * Find the STC8 on the touch bus and, if it answers, poll it on svc. Call once from app_main after touch_init
 * succeeded, since it uses touch's bus; it reads synchronously up to BATTERY_BOOT_TRIES times. A monitor that
 * doesn't answer, or answers junk, is absent: one info line and no polling, and that is ESP_OK. An error means the
 * driver or timer setup failed, and the battery is absent too.
 */
esp_err_t battery_start(void);

/** Whether the STC8 answered at boot. Fixed once battery_start returns. Safe from any task. */
bool battery_present(void);

/** Copy the latest state. Safe from any task; holds a spinlock for the copy only. */
void battery_snapshot(bat_snapshot_t *out);
