#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "proto.h"

/*
 * Assembles the layout pages of one connection into a set (site/src/protocol.md, Pages). Pure: no LVGL, no
 * FreeRTOS, no clock (the caller passes the time), so the host tests compile it.
 *
 * A set is complete when every page from 0 to pages - 1 of one rev is in, in order, with every page naming the same
 * page count. A plain layout is page 0 of 1, so it completes a set of one at once. A page of a newer rev discards a
 * set still arriving. Anything else a correct daemon can't send breaks the set, and the caller drops the link.
 */

#define PAGESET_TIMEOUT_MS 5000 // a set still incomplete this long after its first page is broken

typedef enum {
    PAGESET_PENDING,  // the page is in, and the set waits for more
    PAGESET_COMPLETE, // the set is complete: pages[0 .. n_pages - 1] hold it until the next add or reset
    PAGESET_BROKEN,   // the set is broken and was discarded
} pageset_result_t;

typedef struct {
    proto_layout_t *pages; // PROTO_MAX_PAGES slots the caller owns; reset keeps the pointer
    uint32_t rev;          // of the set arriving, or of the last complete set
    uint8_t n_pages;       // the page count of that set
    uint8_t mask;          // pages received of the set arriving, bit per page; 0 when none is arriving
    bool completed;        // a set has completed on this connection, so rev holds its rev
    int64_t started_ms;    // when the first page of the set arriving came in
} pageset_t;

/** Forget the set arriving and the last complete rev, for a new connection. Keeps `pages`. */
void pageset_reset(pageset_t *ps);

/**
 * Add one parsed page at time `now_ms`. On PAGESET_BROKEN, `*why` says what broke. On PAGESET_PENDING or
 * PAGESET_COMPLETE, `*why` is NULL, or a note that a newer rev discarded the set that was arriving.
 */
pageset_result_t pageset_add(pageset_t *ps, const proto_layout_t *page, int64_t now_ms, const char **why);

/** Whether a set has some of its pages in and waits for the rest. */
bool pageset_arriving(const pageset_t *ps);

/** Whether the set arriving has waited PAGESET_TIMEOUT_MS or more since its first page. */
bool pageset_expired(const pageset_t *ps, int64_t now_ms);
