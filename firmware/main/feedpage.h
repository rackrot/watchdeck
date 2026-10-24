#pragma once

/*
 * The -1 page: stat cards and notes from the latest `feed` snapshot, and the modal that shows one note whole. ui.c owns
 * the page object (it takes the gestures and the touch tracking, as the grid does) and says when to fill it. Everything
 * here runs in the LVGL task or under the LVGL lock; feedpage_fill also needs the model lock when `feed` comes from
 * model_feed().
 */

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "proto.h"

/**
 * Replace the children of `page` with the cards and notes of `feed`: up to 8 cards in two rows of four, then the notes
 * that fit under them (feedfmt_note_rows), a "+N more" line when some don't, or "Nothing here yet" for an empty feed.
 * `now` is the host's clock in Unix seconds for the ages, valid when `have_clock`. A note modal that is open stays up
 * unless `feed` no longer holds its note.
 */
void feedpage_fill(lv_obj_t *page, const proto_feed_t *feed, bool have_clock, uint64_t now);

/** Refresh the age on every note row of the page filled last. Cheap when nothing changed, so ui.c may call it each
 * second. */
void feedpage_tick(bool have_clock, uint64_t now);

/** While the link is down the modal's Dismiss is off. The page's own dimming is ui.c's. */
void feedpage_set_online(bool online);

/**
 * Close the modal and drop the page's state and shared styles. Call after the page object is deleted, and before a
 * new one is filled.
 */
void feedpage_close(void);
