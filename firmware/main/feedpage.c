// The -1 page: see feedpage.h. The cards and rows share a few static styles, made when the page is filled and reset when
// it goes, so the page holds no style memory while it is not up. Text from the feed is never markup: every label here
// has recolor off, and a "#" shows as a "#".

#include "feedpage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "feedfmt.h"
#include "grid.h"
#include "link.h"
#include "model.h"
#include "ui_priv.h"

#define SIDE 12 // the cards and rows keep this much from the page's left and right edge
#define CARD_W 244
#define CARD_COLS FEED_CARDS_PER_ROW
#define ROW_W (GRID_SCREEN_W - 2 * SIDE)
#define ROW_H (FEED_NOTE_H - 4) // the pitch less a 4 px gap
#define EDGE_W 8                // a row's colored edge, by level
#define ROW_PAD 12              // text inset in a card or row
#define TITLE_W 640
#define META_W 300              // source and age, right-aligned
#define MODAL_W 800
#define MODAL_BTN_W 200
#define MODAL_BTN_H 72

static const char *TAG = "feed";

static bool s_styles_ready;
static lv_style_t st_card;         // every card: shape
static lv_style_t st_state[3];     // a card's color by proto_tile_state_t: unknown, ok, bad
static lv_style_t st_card_label;   // 24 pt, centered
static lv_style_t st_card_value;   // 32 pt, centered
static lv_style_t st_row;          // a note row: shape and the left edge's width
static lv_style_t st_level[4];     // the edge's color by proto_level_t
static lv_style_t st_title;        // 24 pt
static lv_style_t st_meta;         // 20 pt, right-aligned: the source and the age
static lv_style_t st_more;         // 24 pt, centered: "+N more"
static lv_style_t st_empty;        // 32 pt, centered: "Nothing here yet"

// The note rows on the page, for the ages and for a tap to find its note. A tap looks the note up by id in the model,
// since a snapshot may have come after the row was drawn.
static struct {
    char id[PROTO_FEED_ID_MAX];
    char src[PROTO_FEED_SRC_MAX];
    uint64_t at;
    lv_obj_t *meta;
    char shown[PROTO_FEED_SRC_MAX + FEED_AGE_MAX + 4]; // what the meta label holds, the source of truth for ui_set_label
} s_rows[PROTO_FEED_NOTES_MAX];
static size_t s_n_rows;

static bool s_online;
static lv_obj_t *s_modal;   // the note modal on the top layer, NULL while closed
static lv_obj_t *s_dismiss; // its Dismiss button
static char s_modal_id[PROTO_FEED_ID_MAX];

static const lv_palette_t LEVEL_PALETTE[] = {LV_PALETTE_BLUE, LV_PALETTE_GREEN, LV_PALETTE_AMBER, LV_PALETTE_RED};

static void style_text(lv_style_t *s, const lv_font_t *font, lv_color_t color, lv_text_align_t align)
{
    lv_style_init(s);
    lv_style_set_text_font(s, font);
    lv_style_set_text_color(s, color);
    lv_style_set_text_align(s, align);
}

static void init_styles(void)
{
    if (s_styles_ready) {
        return;
    }
    s_styles_ready = true;
    lv_style_init(&st_card);
    lv_style_set_radius(&st_card, 12);
    lv_style_set_bg_opa(&st_card, LV_OPA_COVER);
    const lv_color_t state_bg[3] = {lv_palette_darken(LV_PALETTE_GREY, 2), lv_palette_darken(LV_PALETTE_BLUE_GREY, 3),
                                    lv_palette_darken(LV_PALETTE_RED, 2)};
    for (size_t i = 0; i < 3; i++) {
        lv_style_init(&st_state[i]);
        lv_style_set_bg_color(&st_state[i], state_bg[i]);
    }
    style_text(&st_card_label, &lv_font_montserrat_24, lv_palette_lighten(LV_PALETTE_GREY, 3), LV_TEXT_ALIGN_CENTER);
    style_text(&st_card_value, &lv_font_montserrat_32, lv_color_white(), LV_TEXT_ALIGN_CENTER);

    lv_style_init(&st_row);
    lv_style_set_radius(&st_row, 8);
    lv_style_set_bg_opa(&st_row, LV_OPA_COVER);
    lv_style_set_bg_color(&st_row, lv_palette_darken(LV_PALETTE_GREY, 4));
    lv_style_set_border_width(&st_row, EDGE_W);
    lv_style_set_border_side(&st_row, LV_BORDER_SIDE_LEFT);
    for (size_t i = 0; i < 4; i++) {
        lv_style_init(&st_level[i]);
        lv_style_set_border_color(&st_level[i], lv_palette_main(LEVEL_PALETTE[i]));
    }
    style_text(&st_title, &lv_font_montserrat_24, lv_color_white(), LV_TEXT_ALIGN_LEFT);
    style_text(&st_meta, &lv_font_montserrat_20, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_TEXT_ALIGN_RIGHT);
    style_text(&st_more, &lv_font_montserrat_24, lv_palette_lighten(LV_PALETTE_GREY, 1), LV_TEXT_ALIGN_CENTER);
    style_text(&st_empty, &lv_font_montserrat_32, lv_palette_darken(LV_PALETTE_GREY, 1), LV_TEXT_ALIGN_CENTER);
}

static void reset_styles(void)
{
    if (!s_styles_ready) {
        return;
    }
    s_styles_ready = false;
    lv_style_t *all[] = {&st_card, &st_card_label, &st_card_value, &st_row, &st_title, &st_meta, &st_more, &st_empty};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        lv_style_reset(all[i]);
    }
    for (size_t i = 0; i < 3; i++) {
        lv_style_reset(&st_state[i]);
    }
    for (size_t i = 0; i < 4; i++) {
        lv_style_reset(&st_level[i]);
    }
}

// A one-line label of fixed size, ending in dots when the text is too long.
static lv_obj_t *new_label(lv_obj_t *parent, lv_style_t *style, const lv_font_t *font, int x, int y, int w,
                           const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_recolor(label, false);
    lv_obj_add_style(label, style, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, w, lv_font_get_line_height(font));
    lv_label_set_text(label, text);
    return label;
}

// The meta label's text: the source and the age, whichever of them there is.
static void meta_text(char *buf, size_t size, const char *src, const char *age)
{
    snprintf(buf, size, "%s%s%s", src, src[0] && age[0] ? "  " : "", age);
}

static void add_card(lv_obj_t *page, size_t i, const proto_feed_card_t *c)
{
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &st_card, 0);
    lv_obj_add_style(card, &st_state[c->state], 0);
    lv_obj_set_pos(card, SIDE + (int)(i % CARD_COLS) * (CARD_W + FEED_CARD_GAP),
                   FEED_PAD + (int)(i / CARD_COLS) * (FEED_CARD_H + FEED_CARD_GAP));
    lv_obj_set_size(card, CARD_W, FEED_CARD_H);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, false); // a touch on a card lands on the page, which tracks it and takes the swipes

    const int w = CARD_W - 2 * ROW_PAD;
    new_label(card, &st_card_label, &lv_font_montserrat_24, ROW_PAD, 14, w, c->label);

    // The value is 32 pt unless that is wider than the card, then 24 pt.
    lv_point_t size;
    lv_text_get_size(&size, c->value, &lv_font_montserrat_32, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const lv_font_t *font = size.x > w ? &lv_font_montserrat_24 : &lv_font_montserrat_32;
    lv_obj_t *value = new_label(card, &st_card_value, font, ROW_PAD, 14 + lv_font_get_line_height(&lv_font_montserrat_24) + 8,
                                w, c->value);
    if (font != &lv_font_montserrat_32) {
        lv_obj_set_style_text_font(value, font, 0);
    }
}

/* ---- The note modal ---- */

static void close_modal(void)
{
    if (s_modal) {
        lv_obj_delete_async(s_modal); // from an event of its own widgets, so not now
        s_modal = s_dismiss = NULL;
    }
}

static void modal_close_clicked(lv_event_t *e)
{
    (void)e;
    close_modal();
}

static void modal_dismiss_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_online) {
        return;
    }
    ESP_LOGI(TAG, "dismiss %s", s_modal_id);
    if (link_feed_dismiss(s_modal_id) != ESP_OK) {
        ESP_LOGW(TAG, "dismiss %s not queued", s_modal_id);
    }
    close_modal();
}

static lv_obj_t *modal_label(lv_obj_t *card, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_recolor(label, false);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_text(label, text);
    return label;
}

static void modal_button(lv_obj_t *row, const char *text, lv_color_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_size(btn, MODAL_BTN_W, MODAL_BTN_H);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (cb == modal_dismiss_clicked) {
        s_dismiss = btn;
        lv_obj_set_state(btn, LV_STATE_DISABLED, !s_online);
    }
}

// The modal uses local styles only, so it can outlive the page's shared ones for a moment while it is deleted.
static void open_note(const proto_feed_note_t *n, bool have_clock, uint64_t now)
{
    close_modal();
    strlcpy(s_modal_id, n->id, sizeof(s_modal_id));

    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, GRID_SCREEN_W, GRID_SCREEN_H);
    lv_obj_set_scrollable(s_modal, false);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_70, 0);
    lv_obj_set_clickable(s_modal, true); // keeps taps off the page below, and a tap outside the card closes it
    lv_obj_add_event_cb(s_modal, modal_close_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, MODAL_W, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_style_bg_color(card, lv_palette_darken(LV_PALETTE_GREY, 4), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    modal_label(card, &lv_font_montserrat_32, lv_palette_lighten(LEVEL_PALETTE[n->level], 2), n->title);
    char age[FEED_AGE_MAX];
    char meta[PROTO_FEED_SRC_MAX + FEED_AGE_MAX + 4];
    feedfmt_age(age, n->at, now, have_clock);
    meta_text(meta, sizeof(meta), n->src, age);
    modal_label(card, &lv_font_montserrat_20, lv_palette_lighten(LV_PALETTE_GREY, 1), meta);
    if (n->body[0]) {
        modal_label(card, &lv_font_montserrat_24, lv_color_white(), n->body);
    }

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), MODAL_BTN_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    modal_button(row, "Dismiss", lv_palette_darken(LV_PALETTE_RED, 2), modal_dismiss_clicked);
    modal_button(row, "Close", lv_palette_darken(LV_PALETTE_GREY, 2), modal_close_clicked);
}

/* ---- The page ---- */

static void note_tapped(lv_event_t *e)
{
    if (ui_drag_consumed("note")) {
        return;
    }
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_n_rows) {
        return;
    }
    // The LVGL lock is held, so the model lock may follow. Copy the note out, since a snapshot may replace it.
    proto_feed_note_t note = {0};
    bool found = false;
    model_lock();
    const proto_feed_t *f = model_feed();
    for (size_t n = 0; f && n < f->n_notes && !found; n++) {
        if (strcmp(f->notes[n].id, s_rows[i].id) == 0) {
            note = f->notes[n];
            found = true;
        }
    }
    model_unlock();
    if (!found) {
        ESP_LOGW(TAG, "tap on %s ignored: the note is gone", s_rows[i].id);
        return;
    }
    ESP_LOGI(TAG, "note %s opened", note.id);
    uint64_t now = 0;
    const bool have_clock = ui_clock_now(&now);
    open_note(&note, have_clock, now);
}

static void add_note(lv_obj_t *page, size_t i, int y, const proto_feed_note_t *n, bool have_clock, uint64_t now)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &st_row, 0);
    lv_obj_add_style(row, &st_level[n->level], 0);
    lv_obj_set_pos(row, SIDE, y);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_scrollable(row, false);
    lv_obj_add_event_cb(row, ui_touch_track, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(row, ui_touch_track, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(row, note_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    new_label(row, &st_title, &lv_font_montserrat_24, ROW_PAD, (ROW_H - lv_font_get_line_height(&lv_font_montserrat_24)) / 2,
              TITLE_W, n->title);

    strlcpy(s_rows[i].id, n->id, sizeof(s_rows[i].id));
    strlcpy(s_rows[i].src, n->src, sizeof(s_rows[i].src));
    s_rows[i].at = n->at;
    char age[FEED_AGE_MAX];
    feedfmt_age(age, n->at, now, have_clock);
    meta_text(s_rows[i].shown, sizeof(s_rows[i].shown), n->src, age);
    s_rows[i].meta = new_label(row, &st_meta, &lv_font_montserrat_20, ROW_W - EDGE_W - ROW_PAD - META_W,
                               (ROW_H - lv_font_get_line_height(&lv_font_montserrat_20)) / 2, META_W, s_rows[i].shown);
}

void feedpage_fill(lv_obj_t *page, const proto_feed_t *feed, bool have_clock, uint64_t now)
{
    init_styles();
    lv_obj_clean(page);
    s_n_rows = 0;

    for (size_t i = 0; i < feed->n_cards; i++) {
        add_card(page, i, &feed->cards[i]);
    }
    size_t more = 0;
    const size_t rows = feedfmt_note_rows(feed->n_cards, feed->n_notes, &more);
    int y = FEED_PAD;
    if (feed->n_cards > 0) {
        const int card_rows = ((int)feed->n_cards + CARD_COLS - 1) / CARD_COLS;
        y += card_rows * FEED_CARD_H + (card_rows - 1) * FEED_CARD_GAP + FEED_PAD;
    }
    for (size_t i = 0; i < rows; i++) {
        add_note(page, i, y + (int)i * FEED_NOTE_H, &feed->notes[i], have_clock, now);
        s_n_rows++;
    }
    if (more > 0) {
        char text[24];
        snprintf(text, sizeof(text), "+%u more", (unsigned)more);
        new_label(page, &st_more, &lv_font_montserrat_24, 0, y + (int)rows * FEED_NOTE_H + (FEED_MORE_H - SLOT24_H) / 2,
                  GRID_SCREEN_W, text);
    }
    if (feed->n_cards == 0 && feed->n_notes == 0) {
        lv_obj_t *empty = new_label(page, &st_empty, &lv_font_montserrat_32, 0, 0, GRID_SCREEN_W, "Nothing here yet");
        lv_obj_center(empty);
    }
    ESP_LOGI(TAG, "feed page: %u card%s, %u of %u note%s", (unsigned)feed->n_cards, feed->n_cards == 1 ? "" : "s",
             (unsigned)rows, (unsigned)feed->n_notes, feed->n_notes == 1 ? "" : "s");

    if (s_modal) {
        bool there = false;
        for (size_t i = 0; i < feed->n_notes && !there; i++) {
            there = strcmp(feed->notes[i].id, s_modal_id) == 0;
        }
        if (!there) {
            close_modal(); // dismissed elsewhere, or expired
        }
    }
}

void feedpage_tick(bool have_clock, uint64_t now)
{
    for (size_t i = 0; i < s_n_rows; i++) {
        char age[FEED_AGE_MAX];
        char text[sizeof(s_rows[i].shown)];
        feedfmt_age(age, s_rows[i].at, now, have_clock);
        meta_text(text, sizeof(text), s_rows[i].src, age);
        ui_set_label(s_rows[i].meta, s_rows[i].shown, sizeof(s_rows[i].shown), text);
    }
}

void feedpage_set_online(bool online)
{
    s_online = online;
    if (s_dismiss) {
        lv_obj_set_state(s_dismiss, LV_STATE_DISABLED, !online);
    }
}

void feedpage_close(void)
{
    close_modal();
    s_n_rows = 0;
    reset_styles();
}
