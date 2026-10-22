#include "hoststat.h"

#include <stdio.h>
#include <time.h>

// gmtime_r on a 32-bit time_t would fail after 2038.
_Static_assert(sizeof(time_t) == 8, "the clock must reach past 2038");

bool hoststat_fresh(bool have_frame, int64_t elapsed_us)
{
    return have_frame && elapsed_us <= HOSTSTAT_STALE_US;
}

int64_t hoststat_local_time(uint64_t frame_time, int32_t utc_offset, int64_t elapsed_us)
{
    return (int64_t)frame_time + utc_offset + (elapsed_us > 0 ? elapsed_us / 1000000 : 0);
}

void hoststat_cpu(char *buf, size_t size, unsigned cpu)
{
    snprintf(buf, size, "%u%%", cpu > 100 ? 100u : cpu);
}

void hoststat_mem(char *buf, size_t size, uint64_t used, uint64_t total)
{
    if (total == 0) {
        snprintf(buf, size, "--");
        return;
    }
    unsigned pct;
    if (used >= total) {
        pct = 100;
    } else if (used <= UINT64_MAX / 100) {
        pct = (unsigned)(used * 100 / total);
    } else {
        pct = (unsigned)(used / (total / 100)); // a total over 2^64 / 100 bytes: coarser, but cannot overflow
        if (pct > 100) {
            pct = 100;
        }
    }
    snprintf(buf, size, "%u%%", pct);
}

void hoststat_rate(char *buf, size_t size, uint64_t bytes_per_s)
{
    if (bytes_per_s < 1000) {
        snprintf(buf, size, "%u", (unsigned)bytes_per_s);
        return;
    }
    static const char units[] = "kMG";
    uint64_t scale = 1000;
    int unit = 0;
    while (unit < 2 && bytes_per_s >= scale * 1000) {
        scale *= 1000;
        unit++;
    }
    uint64_t tenths = bytes_per_s / (scale / 10); // truncated
    if (tenths > 9999) {
        tenths = 9999; // "999G"
    }
    if (tenths < 100) {
        snprintf(buf, size, "%u.%u%c", (unsigned)(tenths / 10), (unsigned)(tenths % 10), units[unit]);
    } else {
        snprintf(buf, size, "%u%c", (unsigned)(tenths / 10), units[unit]);
    }
}

void hoststat_clock(char *buf, size_t size, int64_t local_unix)
{
    static const char *const days[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const time_t t = (time_t)local_unix;
    struct tm tm;
    if (!gmtime_r(&t, &tm) || tm.tm_wday < 0 || tm.tm_wday > 6 || tm.tm_mon < 0 || tm.tm_mon > 11) {
        snprintf(buf, size, "--:--");
        return;
    }
    snprintf(buf, size, "%s %02d %s %02d:%02d", days[tm.tm_wday], tm.tm_mday % 100, months[tm.tm_mon], tm.tm_hour % 100,
             tm.tm_min % 100);
}
