#pragma once

/*
 * Text for the status bar's host values, with no LVGL so that a host test can pin it (test_hoststat.c). ui.c adds the
 * labels ("CPU ", "RAM ") and the rate arrows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto.h"

#define HOSTSTAT_TEXT_MAX 24  // room for any single value below, and its NUL
#define HOSTSTAT_GROUP_MAX 64 // room for any group below with color markup around each value, and its NUL
#define HOSTSTAT_VALUE_W 4    // the longest percent text, "100%": the bar pads each to this width
#define HOSTSTAT_TEMP_W 3     // the longest temperature number, "-40", "150" or "302": the unit follows ("°C")

#define HOSTSTAT_STALE_US (5 * 1000000LL) // host values show "--" once the latest frame is older than this

/** Whether host values may be shown: a frame came, and `elapsed_us` since it is at most HOSTSTAT_STALE_US. */
bool hoststat_fresh(bool have_frame, int64_t elapsed_us);

/** The host's local time now, in Unix-style seconds: the frame's `time` plus `utc_offset` plus whole elapsed seconds.
 */
int64_t hoststat_local_time(uint64_t frame_time, int32_t utc_offset, int64_t elapsed_us);

/** "12%", for CPU and GPU busy. Above 100 shows 100%. */
void hoststat_percent(char *buf, size_t size, unsigned percent);

/** A whole °C as the bar shows it in `unit`, number only: "64", or "147" in °F (the nearest whole number). */
void hoststat_temp(char *buf, size_t size, int temp_c, proto_temp_unit_t unit);

/** Memory used as a percent of the total, "45%", truncated; "--" when `total` is 0. Used above total shows 100%. */
void hoststat_mem(char *buf, size_t size, uint64_t used, uint64_t total);

/** The cells a rate in `unit` is right-aligned in: 6 for SI ("999.9M"), 7 for binary ("999.9Mi") and bits ("999.9Mb").
 */
int hoststat_rate_width(proto_net_unit_t unit);

/**
 * Bytes per second in `unit`: a whole number under 1000 ("999"; bits add a "b"), else one decimal and a prefix: "1.0k",
 * "999.9M", "2.5G" (SI), "0.9Mi" (binary), "9.6Mb" (bits, the bytes times 8). SI and bits divide by 1000, binary by
 * 1024, and every unit switches prefix at 1000, so the number never has a fourth integer digit. Truncates, so 999,999
 * shows "999.9k" and never "1000.0k". Caps at "999.9G" with the unit's suffix. At most hoststat_rate_width()
 * characters.
 */
void hoststat_rate(char *buf, size_t size, uint64_t bytes_per_s, proto_net_unit_t unit);

/*
 * The four host groups of the bar, each a fixed number of cells, ready for a label with recolor on. A value the frame
 * lacks, or any value once `fresh` is false, is "--" in its cells and never colored. A value with a color in
 * `colors_mask` is wrapped as "#rrggbb 97%#", the padding outside the markup; the prefixes and the units stay
 * uncolored. Each result is at most HOSTSTAT_GROUP_MAX bytes with its NUL.
 */

/** "CPU  12%  64°C": busy and temperature. */
void hoststat_group_cpu(char *buf, size_t size, const proto_stats_t *stats, bool fresh);

/** "RAM  45%". */
void hoststat_group_mem(char *buf, size_t size, const proto_stats_t *stats, bool fresh);

/** "GPU  23%  34°C": busy and temperature, either of which may be absent. */
void hoststat_group_gpu(char *buf, size_t size, const proto_stats_t *stats, bool fresh);

/** `down` + rx + " " + `up` + tx, each rate right-aligned in hoststat_rate_width(). The caller gives the arrows. */
void hoststat_group_net(char *buf, size_t size, const proto_stats_t *stats, bool fresh, const char *down,
                        const char *up);

/** "Wed 22 Oct 14:32", 24-hour, for `local_unix` seconds since the epoch in the wanted zone. "--:--" if out of range.
 */
void hoststat_clock(char *buf, size_t size, int64_t local_unix);
