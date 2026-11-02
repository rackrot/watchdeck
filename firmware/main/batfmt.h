#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The battery monitor's pure parts: decoding the STC8's registers, deciding what a reading means, how fresh it is and
 * when it goes to the daemon. No driver and no LVGL, so the host tests build it. The register map is in
 * site/src/device-spec.md "Battery monitoring".
 */

#define BATFMT_REGS 11                    // registers 0x00 to 0x0A, read in one transfer
#define BAT_STALE_US (15 * 1000 * 1000LL) // three polls
#define BAT_MV_MAX 6000
#define BAT_SEND_PERCENT_STEP 2
#define BAT_LEVELS 5         // batfmt_level returns 0 to BAT_LEVELS - 1
#define BATFMT_PERCENT_MAX 5 // "100%" and the NUL
#define BATFMT_ROW_MAX 40

typedef enum {
    BAT_UNKNOWN, // both status bits low: the STC8 says nothing usable
    BAT_CHARGING,
    BAT_FULL,
    BAT_DISCHARGING,
    BAT_NONE, // no cell attached; batfmt_decode never gives it (plan A4, waits on C2)
} bat_charge_t;

typedef struct {
    uint32_t pin_mv;     // the STC8's ADC pin
    uint32_t mv;         // the battery
    uint8_t percent;     // 0 to 100, the STC8's own
    uint8_t status;      // register 0x09, raw
    uint8_t status_copy; // register 0x0A, raw (it repeats 0x09; only logged)
    bat_charge_t charge;
} bat_reading_t;

/**
 * Decode the 11 bytes read from register 0x00 into `out`. Returns false, leaving `out` alone, when the buffer is junk
 * and not a reading: the pin mV is 0, the battery mV is not pin x 139 / 100 within 2 % + 20 mV, the battery mV is over
 * BAT_MV_MAX, the percent is over 100 (refused, not clamped), or the status is over 0x03.
 */
bool batfmt_decode(const uint8_t regs[BATFMT_REGS], bat_reading_t *out);

/** Whether a reading taken `age_us` ago is still shown: `have` and no older than BAT_STALE_US. */
bool batfmt_fresh(bool have, int64_t age_us);

/** The glyph step for a percent, 0 (empty) to BAT_LEVELS - 1 (full). A percent over 100 counts as 100. */
int batfmt_level(unsigned percent);

/**
 * Whether the link sends a `battery` frame. `sent` is what the session's last frame carried and `now` is what the
 * battery says now; NULL means unavailable. `have_sent` is false before the session's first frame. True for the first
 * frame, when availability flips (even back to the very reading sent before it went away), when the charge state
 * differs, and when the percent is BAT_SEND_PERCENT_STEP or more from `sent`'s, not from the last one seen.
 */
bool batfmt_should_send(bool have_sent, const bat_reading_t *sent, const bat_reading_t *now);

/** The bar's percent text, right-aligned in 4 cells, or "--" when `r` is NULL. */
void batfmt_percent(char out[BATFMT_PERCENT_MAX], const bat_reading_t *r);

/** The Settings > Status row: voltage, percent and state, or "Battery --" when `r` is NULL. */
void batfmt_row(char *out, size_t size, const bat_reading_t *r);

/** The wire name of a charge state, as in the `battery` frame's `charge`. */
const char *batfmt_charge_name(bat_charge_t charge);
