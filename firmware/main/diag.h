#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Counters for the failures the Errors tab lists, each with the reason of the latest one. Pure but for a FreeRTOS
 * critical section (the linux target has one), so the host tests compile it. Any task may call diag_count.
 */

typedef enum {
    DIAG_WIFI_DROP,    // a Wi-Fi disconnect; the reason is wifi_reason_name of its code
    DIAG_LINK_DROP,    // the link to the daemon lost or failed
    DIAG_SET_BROKEN,   // a layout set the daemon sent that could not be assembled
    DIAG_ICON_TIMEOUT, // icon_get left unanswered
    DIAG_CARD_ERROR,   // a failed TF card read, write, mount or recovery, or a refused card write
    DIAG_KINDS,
} diag_kind_t;

#define DIAG_REASON_MAX 48 // bytes of a stored reason, NUL included

typedef struct {
    uint32_t count;
    char reason[DIAG_REASON_MAX]; // empty until the first count
} diag_entry_t;

// Count one event of `kind` with its reason (copied and cut to DIAG_REASON_MAX - 1). NULL reason keeps the last one.
void diag_count(diag_kind_t kind, const char *reason);

// Copy the entry for `kind`. Returns false for an unknown kind.
bool diag_get(diag_kind_t kind, diag_entry_t *out);

// A short label for the tab: "Wi-Fi drops", "Link drops", ...
const char *diag_kind_name(diag_kind_t kind);

// What a WIFI_REASON_* code means to a person. Unknown codes read "other".
const char *wifi_reason_name(int code);
