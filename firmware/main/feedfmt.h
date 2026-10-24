#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The feed page's pure parts: the age text and how many note rows fit. No LVGL, so the host tests build it. */

// The page is GRID_SCREEN_H - GRID_TOP = 556 px tall. Every size here is in px.
#define FEED_AREA_H 556
#define FEED_PAD 8     // above the first card or row, and between a card row and the next block
#define FEED_CARD_H 112
#define FEED_CARD_GAP 8
#define FEED_CARDS_PER_ROW 4
#define FEED_NOTE_H 56 // a note row's pitch: 52 px of row and 4 px of gap
#define FEED_MORE_H 32 // the "+N more" line
#define FEED_AGE_MAX 12 // "now", "59 min", "23 h", "3 d" and the NUL

/**
 * Write how long ago `at` was, from the host's clock `now` (both Unix seconds): "now" under a minute, then "N min",
 * "N h" and "N d". Empty when the clock is unknown (`have_clock` false) or `at` is 0 (the daemon didn't know it). An
 * `at` in the future counts as "now".
 */
void feedfmt_age(char out[FEED_AGE_MAX], uint64_t at, uint64_t now, bool have_clock);

/**
 * How many of `n_notes` note rows the page shows under `n_cards` cards. When they don't all fit, the last row's space
 * goes to a "+N more" line, and `*more` is N. Otherwise `*more` is 0.
 */
size_t feedfmt_note_rows(size_t n_cards, size_t n_notes, size_t *more);
