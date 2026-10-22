#pragma once

/*
 * Text for the status bar's host values, with no LVGL so that a host test can pin it (test_hoststat.c). ui.c adds the
 * labels ("CPU ", "MEM ") and the rate arrows.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOSTSTAT_TEXT_MAX 24 // room for any result below, and its NUL
#define HOSTSTAT_VALUE_W 4   // the longest cpu, mem or rate text, "100%" or "999k": the bar pads each to this width

#define HOSTSTAT_STALE_US (5 * 1000000LL) // host values show "--" once the latest frame is older than this

/** Whether host values may be shown: a frame came, and `elapsed_us` since it is at most HOSTSTAT_STALE_US. */
bool hoststat_fresh(bool have_frame, int64_t elapsed_us);

/** The host's local time now, in Unix-style seconds: the frame's `time` plus `utc_offset` plus whole elapsed seconds.
 */
int64_t hoststat_local_time(uint64_t frame_time, int32_t utc_offset, int64_t elapsed_us);

/** "12%". Above 100 shows 100%. */
void hoststat_cpu(char *buf, size_t size, unsigned cpu);

/** Memory used as a percent of the total, "45%", truncated; "--" when `total` is 0. Used above total shows 100%. */
void hoststat_mem(char *buf, size_t size, uint64_t used, uint64_t total);

/**
 * Bytes per second with an SI suffix and no unit: "999", "1.0k", "999k", "1.0M", "12M", "2.5G". Truncates, so 999,999
 * shows "999k" and never "1000k". Caps at "999G".
 */
void hoststat_rate(char *buf, size_t size, uint64_t bytes_per_s);

/** "Wed 22 Oct 14:32", 24-hour, for `local_unix` seconds since the epoch in the wanted zone. "--:--" if out of range.
 */
void hoststat_clock(char *buf, size_t size, int64_t local_unix);
