#include "batfmt.h"

#include <stdio.h>

// Register 0x09: bit 0 is STDBY and bit 1 is CHRG, both active low (plan A3; "full" is untested on the board).
static bat_charge_t charge_of(uint8_t status)
{
    switch (status) {
    case 0x01:
        return BAT_CHARGING;
    case 0x02:
        return BAT_FULL;
    case 0x03:
        return BAT_DISCHARGING;
    default:
        return BAT_UNKNOWN;
    }
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool batfmt_decode(const uint8_t regs[BATFMT_REGS], bat_reading_t *out)
{
    const uint32_t pin = le32(regs);
    const uint32_t mv = le32(regs + 4);
    if (pin == 0 || mv > BAT_MV_MAX || regs[8] > 100 || regs[9] > 0x03) {
        return false;
    }
    // The divider is 39k/100k. 64 bits, so a pin of 0xFFFFFFFF can't wrap.
    const int64_t want = (int64_t)pin * 139 / 100;
    const int64_t tolerance = want * 2 / 100 + 20;
    const int64_t diff = (int64_t)mv - want;
    if (diff > tolerance || -diff > tolerance) {
        return false;
    }
    out->pin_mv = pin;
    out->mv = mv;
    out->percent = regs[8];
    out->status = regs[9];
    out->status_copy = regs[10];
    out->charge = charge_of(regs[9]);
    return true;
}

bool batfmt_fresh(bool have, int64_t age_us)
{
    return have && age_us <= BAT_STALE_US;
}

int batfmt_level(unsigned percent)
{
    if (percent > 100) {
        percent = 100;
    }
    return (int)(percent * (BAT_LEVELS - 1) / 100);
}

bool batfmt_should_send(bool have_sent, const bat_reading_t *sent, const bat_reading_t *now)
{
    if (!have_sent || (sent == NULL) != (now == NULL)) {
        return true;
    }
    if (now == NULL) {
        return false;
    }
    const int moved = (int)now->percent - (int)sent->percent;
    return now->charge != sent->charge || moved >= BAT_SEND_PERCENT_STEP || -moved >= BAT_SEND_PERCENT_STEP;
}

const char *batfmt_charge_name(bat_charge_t charge)
{
    switch (charge) {
    case BAT_CHARGING:
        return "charging";
    case BAT_FULL:
        return "full";
    case BAT_DISCHARGING:
        return "discharging";
    case BAT_NONE:
        return "none";
    default:
        return "unknown";
    }
}

bool batfmt_percent(char *out, size_t size, const bat_reading_t *r)
{
    const int n = r == NULL ? snprintf(out, size, "--") : snprintf(out, size, "%3u%%", (unsigned)r->percent % 1000);
    return n >= 0 && (size_t)n < size;
}

bool batfmt_row(char *out, size_t size, const bat_reading_t *r)
{
    int n;
    if (r == NULL) {
        n = snprintf(out, size, "Battery --");
    } else if (r->charge == BAT_NONE) {
        n = snprintf(out, size, "Battery no battery");
    } else {
        n = snprintf(out, size, "Battery %u.%02u V, %u%%, %s", (unsigned)(r->mv / 1000 % 100),
                     (unsigned)(r->mv % 1000 / 10), (unsigned)r->percent % 1000, batfmt_charge_name(r->charge));
    }
    return n >= 0 && (size_t)n < size;
}
