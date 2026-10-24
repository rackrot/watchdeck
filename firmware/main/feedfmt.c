#include "feedfmt.h"

#include <stdio.h>

void feedfmt_age(char out[FEED_AGE_MAX], uint64_t at, uint64_t now, bool have_clock)
{
    if (!have_clock || at == 0) {
        out[0] = '\0';
        return;
    }
    const uint64_t s = now > at ? now - at : 0;
    if (s < 60) {
        snprintf(out, FEED_AGE_MAX, "now");
    } else if (s < 3600) {
        snprintf(out, FEED_AGE_MAX, "%u min", (unsigned)(s / 60));
    } else if (s < 86400) {
        snprintf(out, FEED_AGE_MAX, "%u h", (unsigned)(s / 3600));
    } else {
        const uint64_t d = s / 86400;
        snprintf(out, FEED_AGE_MAX, "%u d", (unsigned)(d > 999 ? 999 : d));
    }
}

size_t feedfmt_note_rows(size_t n_cards, size_t n_notes, size_t *more)
{
    int below = FEED_AREA_H - FEED_PAD;
    if (n_cards > 0) {
        const int card_rows = (int)((n_cards + FEED_CARDS_PER_ROW - 1) / FEED_CARDS_PER_ROW);
        below -= card_rows * FEED_CARD_H + (card_rows - 1) * FEED_CARD_GAP + FEED_PAD;
    }
    const size_t fit = below > 0 ? (size_t)(below / FEED_NOTE_H) : 0;
    if (n_notes <= fit) {
        *more = 0;
        return n_notes;
    }
    const size_t rows = below > FEED_MORE_H ? (size_t)((below - FEED_MORE_H) / FEED_NOTE_H) : 0;
    *more = n_notes - rows;
    return rows;
}
