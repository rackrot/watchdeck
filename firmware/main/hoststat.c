#include "hoststat.h"

#include <stdio.h>
#include <string.h>
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

void hoststat_percent(char *buf, size_t size, unsigned percent)
{
    snprintf(buf, size, "%u%%", percent > 100 ? 100u : percent);
}

void hoststat_temp(char *buf, size_t size, int temp_c, proto_temp_unit_t unit)
{
    if (unit == PROTO_TEMP_F) {
        // c * 9 / 5 + 32 in tenths is 18c + 320, which is even, so its tenths digit is never 5 and no tie occurs.
        const int tenths = 18 * temp_c + 320;
        temp_c = tenths >= 0 ? (tenths + 5) / 10 : -((-tenths + 5) / 10);
    }
    snprintf(buf, size, "%d", temp_c);
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

int hoststat_rate_width(proto_net_unit_t unit)
{
    return unit == PROTO_NET_SI ? 6 : 7;
}

void hoststat_rate(char *buf, size_t size, uint64_t bytes_per_s, proto_net_unit_t unit)
{
    static const char *const si[] = {"", "k", "M", "G"};
    static const char *const binary[] = {"", "Ki", "Mi", "Gi"};
    uint64_t value = bytes_per_s;
    uint64_t step = 1000;
    const char *const *prefix = si;
    const char *suffix = "";
    if (unit == PROTO_NET_BINARY) {
        step = 1024;
        prefix = binary;
    } else if (unit == PROTO_NET_BITS) {
        value = bytes_per_s > UINT64_MAX / 8 ? UINT64_MAX : bytes_per_s * 8;
        suffix = "b";
    }
    if (value < 1000) {
        snprintf(buf, size, "%u%s", (unsigned)value, suffix);
        return;
    }
    // Divide until the integer part is under 1000, or the prefixes run out.
    uint64_t div = 1;
    int i = 0;
    while (i < 3 && value / div >= 1000) {
        div *= step;
        i++;
    }
    // Tenths, truncated, without value * 10 overflowing.
    uint64_t tenths = value / div * 10 + value % div * 10 / div;
    if (tenths > 9999) {
        tenths = 9999; // "999.9G"
    }
    snprintf(buf, size, "%u.%u%s%s", (unsigned)(tenths / 10), (unsigned)(tenths % 10), prefix[i], suffix);
}

// The color of value `which`, or -1 when the frame gave it none. Only asked for values that exist.
static int64_t color_of(const proto_stats_t *stats, proto_color_t which)
{
    return (stats->colors_mask >> which) & 1 ? (int64_t)(stats->colors[which] & 0xFFFFFF) : -1;
}

// Appends `text` right-aligned in `width` cells to buf at `off` and returns the new end. With a color the text is
// wrapped in LVGL's recolor markup, "#rrggbb text#", and the padding stays outside it.
static size_t cell(char *buf, size_t size, size_t off, int width, const char *text, int64_t color)
{
    if (off >= size) {
        return off;
    }
    const int pad = width > (int)strlen(text) ? width - (int)strlen(text) : 0;
    int n;
    if (color < 0) {
        n = snprintf(buf + off, size - off, "%*s%s", pad, "", text);
    } else {
        n = snprintf(buf + off, size - off, "%*s#%06x %s#", pad, "", (unsigned)color, text);
    }
    return n < 0 ? off : (off + (size_t)n < size ? off + (size_t)n : size - 1);
}

// "<label>  12%  64°C" for CPU and GPU. A value that is absent or stale is "--" in its cells.
static void busy_temp_group(char *buf, size_t size, const char *label, const proto_stats_t *stats, bool fresh,
                            bool has_busy, unsigned busy, proto_color_t busy_color, bool has_temp, int temp,
                            proto_color_t temp_color)
{
    char text[HOSTSTAT_TEXT_MAX];
    size_t off = 0;
    buf[0] = '\0';
    off = cell(buf, size, off, 0, label, -1);
    off = cell(buf, size, off, 0, " ", -1);

    const bool busy_ok = fresh && has_busy;
    if (busy_ok) {
        hoststat_percent(text, sizeof(text), busy);
    }
    off = cell(buf, size, off, HOSTSTAT_VALUE_W, busy_ok ? text : "--", busy_ok ? color_of(stats, busy_color) : -1);
    off = cell(buf, size, off, 0, " ", -1);

    const bool temp_ok = fresh && has_temp;
    if (temp_ok) {
        hoststat_temp(text, sizeof(text), temp, stats->temp_unit);
    }
    off = cell(buf, size, off, HOSTSTAT_TEMP_W, temp_ok ? text : "--", temp_ok ? color_of(stats, temp_color) : -1);
    cell(buf, size, off, 0,
         stats->temp_unit == PROTO_TEMP_F ? "\xC2\xB0"
                                            "F"
                                          : "\xC2\xB0"
                                            "C",
         -1);
}

void hoststat_group_cpu(char *buf, size_t size, const proto_stats_t *stats, bool fresh)
{
    busy_temp_group(buf, size, "CPU", stats, fresh, stats->has_cpu, stats->cpu, PROTO_COLOR_CPU, stats->has_cpu_temp,
                    stats->cpu_temp, PROTO_COLOR_CPU_TEMP);
}

void hoststat_group_gpu(char *buf, size_t size, const proto_stats_t *stats, bool fresh)
{
    busy_temp_group(buf, size, "GPU", stats, fresh, stats->has_gpu, stats->gpu, PROTO_COLOR_GPU, stats->has_gpu_temp,
                    stats->gpu_temp, PROTO_COLOR_GPU_TEMP);
}

void hoststat_group_mem(char *buf, size_t size, const proto_stats_t *stats, bool fresh)
{
    char text[HOSTSTAT_TEXT_MAX];
    const bool ok = fresh && stats->has_mem && stats->mem_total > 0;
    if (ok) {
        hoststat_mem(text, sizeof(text), stats->mem_used, stats->mem_total);
    }
    buf[0] = '\0';
    size_t off = cell(buf, size, 0, 0, "RAM ", -1);
    cell(buf, size, off, HOSTSTAT_VALUE_W, ok ? text : "--", ok ? color_of(stats, PROTO_COLOR_MEM) : -1);
}

void hoststat_group_net(char *buf, size_t size, const proto_stats_t *stats, bool fresh, const char *down,
                        const char *up)
{
    char rx[HOSTSTAT_TEXT_MAX], tx[HOSTSTAT_TEXT_MAX];
    const bool ok = fresh && stats->has_net;
    if (ok) {
        hoststat_rate(rx, sizeof(rx), stats->net_rx, stats->net_unit);
        hoststat_rate(tx, sizeof(tx), stats->net_tx, stats->net_unit);
    }
    const int w = hoststat_rate_width(stats->net_unit);
    buf[0] = '\0';
    size_t off = cell(buf, size, 0, 0, down, -1);
    off = cell(buf, size, off, w, ok ? rx : "--", ok ? color_of(stats, PROTO_COLOR_NET_RX) : -1);
    off = cell(buf, size, off, 0, " ", -1);
    off = cell(buf, size, off, 0, up, -1);
    cell(buf, size, off, w, ok ? tx : "--", ok ? color_of(stats, PROTO_COLOR_NET_TX) : -1);
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
