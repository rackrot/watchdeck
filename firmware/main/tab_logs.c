// The Logs tab: the log ring, 15 lines a page, with Oldest, Older, Newer and Newest, Pause, and an "Errors only"
// switch. A page is copied into a PSRAM buffer the tab owns and the labels point at that copy, so the text costs the
// LVGL pool nothing. Pages are named by sequence numbers, so lines arriving while a page is read don't move it.
// Everything runs in the LVGL task or under the LVGL lock.

#include <stdint.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "logcap.h"
#include "settings.h"

#define PAGE_LINES 15
#define CTL_H 48
#define BTN_W 112
#define GAP 8
#define LINES_Y (CTL_H + 2 * GAP) // the lines start under the control row
#define POS_TEXT_MAX 24

_Static_assert(LINES_Y + PAGE_LINES * LOG_LINE_H <= SETTINGS_CONTENT_H, "15 lines fit under the controls");

typedef enum {
    BTN_OLDEST,
    BTN_OLDER,
    BTN_NEWER,
    BTN_NEWEST,
    BTN_PAUSE,
    BTN_COUNT,
} btn_t;

typedef enum {
    MOVE_NEWEST,
    MOVE_OLDEST,
    MOVE_OLDER,
    MOVE_NEWER,
} move_t;

static const char *const BTN_NAMES[BTN_COUNT] = {"Oldest", "Older", "Newer", "Newest", "Pause"};

static logring_line_t *s_lines; // PAGE_LINES lines in PSRAM, NULL while the tab is not shown
static size_t s_n;              // lines on the page
static lv_obj_t *s_labels[PAGE_LINES];
static lv_obj_t *s_pos;
static lv_obj_t *s_pause_label;
static char s_pos_text[POS_TEXT_MAX];
static bool s_errors_only;
static bool s_follow;        // the page is the newest one and moves on as lines arrive
static bool s_paused;        // follow is held
static uint32_t s_seen_next; // logcap_next_seq() when the page was read

static void show_page(void)
{
    for (size_t i = 0; i < PAGE_LINES; i++) {
        settings_log_line_set(s_labels[i], i < s_n ? &s_lines[i] : NULL);
    }
    if (s_n == 0) {
        lv_label_set_text_static(s_labels[0], "(no lines)");
        ui_set_label(s_pos, s_pos_text, sizeof(s_pos_text), "");
        return;
    }
    char pos[POS_TEXT_MAX];
    snprintf(pos, sizeof(pos), "#%u-%u", (unsigned)s_lines[0].seq, (unsigned)s_lines[s_n - 1].seq);
    ui_set_label(s_pos, s_pos_text, sizeof(s_pos_text), pos);
}

static void load(move_t move)
{
    const uint32_t next = logcap_next_seq();
    size_t n = 0;
    if (move == MOVE_OLDER) {
        n = s_n ? logcap_read_before(s_lines[0].seq, s_errors_only, s_lines, PAGE_LINES) : 0;
        if (n == 0) {
            return; // already at the oldest line
        }
        s_follow = false;
    } else if (move == MOVE_NEWER) {
        n = s_n ? logcap_read_from(s_lines[s_n - 1].seq + 1, s_errors_only, s_lines, PAGE_LINES) : 0;
        if (n == PAGE_LINES) {
            s_follow = s_lines[n - 1].seq + 1 == next; // a full page that ends at the newest line follows on
        } else {
            move = MOVE_NEWEST; // fewer than a page after it: that is the end, so show the last page
        }
    } else if (move == MOVE_OLDEST) {
        n = logcap_read_from(logcap_oldest_seq(), s_errors_only, s_lines, PAGE_LINES);
        s_follow = false;
    }
    if (move == MOVE_NEWEST) {
        n = logcap_read_before(next, s_errors_only, s_lines, PAGE_LINES);
        s_follow = true;
    }
    s_n = n;
    s_seen_next = next;
    show_page();
}

static void button_tapped(lv_event_t *e)
{
    switch ((btn_t)(intptr_t)lv_event_get_user_data(e)) {
    case BTN_OLDEST:
        load(MOVE_OLDEST);
        break;
    case BTN_OLDER:
        load(MOVE_OLDER);
        break;
    case BTN_NEWER:
        load(MOVE_NEWER);
        break;
    case BTN_NEWEST:
        s_paused = false;
        lv_label_set_text_static(s_pause_label, BTN_NAMES[BTN_PAUSE]);
        load(MOVE_NEWEST);
        break;
    case BTN_PAUSE:
        s_paused = !s_paused;
        lv_label_set_text_static(s_pause_label, s_paused ? "Follow" : BTN_NAMES[BTN_PAUSE]);
        break;
    default:
        break;
    }
}

static void errors_toggled(lv_event_t *e)
{
    s_errors_only = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
    load(MOVE_NEWEST);
}

static void line_tapped(lv_event_t *e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i < s_n) {
        settings_log_line_open(&s_lines[i]);
    }
}

void tab_logs_build(lv_obj_t *parent)
{
    s_lines = heap_caps_calloc(PAGE_LINES, sizeof(*s_lines), MALLOC_CAP_SPIRAM);
    if (!s_lines) {
        lv_obj_t *label = lv_label_create(parent);
        lv_label_set_text_static(label, "Out of memory for the log page");
        lv_obj_set_pos(label, SETTINGS_PAD, SETTINGS_PAD);
        return;
    }
    s_n = 0;
    s_pos_text[0] = '\0';
    s_paused = false;

    for (int i = 0; i < BTN_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(parent);
        lv_obj_set_size(btn, BTN_W, CTL_H);
        lv_obj_set_pos(btn, SETTINGS_PAD + i * (BTN_W + GAP), GAP / 2);
        lv_obj_add_event_cb(btn, button_tapped, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text_static(label, BTN_NAMES[i]);
        lv_obj_center(label);
        if (i == BTN_PAUSE) {
            s_pause_label = label;
        }
    }

    const int sw_x = SETTINGS_PAD + BTN_COUNT * (BTN_W + GAP) + GAP;
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 64, 34);
    lv_obj_set_pos(sw, sw_x, GAP / 2 + (CTL_H - 34) / 2);
    if (s_errors_only) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, errors_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *sw_label = lv_label_create(parent);
    lv_obj_set_style_text_font(sw_label, &lv_font_montserrat_20, 0);
    lv_label_set_text_static(sw_label, "Errors only");
    lv_obj_set_pos(sw_label, sw_x + 72, GAP / 2 + (CTL_H - 23) / 2);

    const int pos_x = sw_x + 72 + 130;
    s_pos = lv_label_create(parent);
    lv_obj_set_style_text_font(s_pos, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_pos, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_pos, SETTINGS_CONTENT_W - SETTINGS_PAD - pos_x);
    lv_obj_set_style_text_align(s_pos, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_pos, pos_x, GAP / 2 + (CTL_H - 23) / 2);
    lv_label_set_text(s_pos, "");

    for (size_t i = 0; i < PAGE_LINES; i++) {
        s_labels[i] = settings_log_line_create(parent, SETTINGS_PAD, LINES_Y + (int)i * LOG_LINE_H,
                                               SETTINGS_CONTENT_W - 2 * SETTINGS_PAD, LOG_LINE_H, line_tapped,
                                               (void *)(intptr_t)i);
    }
    load(MOVE_NEWEST);
    ui_log_heap("logs tab built");
}

// Follow new lines once a second on the newest page, unless paused or nothing arrived.
void tab_logs_tick(void)
{
    if (s_lines && s_follow && !s_paused && logcap_next_seq() != s_seen_next) {
        load(MOVE_NEWEST);
    }
}

void tab_logs_leave(void)
{
    settings_log_line_close();
    heap_caps_free(s_lines);
    s_lines = NULL;
    s_n = 0;
    s_pos = s_pause_label = NULL;
    for (size_t i = 0; i < PAGE_LINES; i++) {
        s_labels[i] = NULL;
    }
}
