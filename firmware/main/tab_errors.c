// The Errors tab: the pending crash or reset report, this boot's reset reason, the failure counters with their last
// reasons, and the last 12 error and warning lines of the log, each opening whole on a tap. Everything runs in the LVGL
// task or under the LVGL lock.

#include <stdio.h>
#include <string.h>

#include "crash.h"
#include "diag.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "logcap.h"
#include "settings.h"

#define ROW_H 31 // Montserrat 24's 27 px line and 4 px
#define TOP_Y 4
#define ROW_TEXT_MAX 112
#define RECENT_LINES 12
#define LEFT_X SETTINGS_PAD
#define LEFT_W 500
#define RIGHT_X (SETTINGS_PAD + LEFT_W + SETTINGS_PAD)
#define RIGHT_W (SETTINGS_CONTENT_W - SETTINGS_PAD - RIGHT_X)
#define TOP_ROWS DIAG_KINDS // the counters set the height of the top block, and the report rows are fewer
#define HEAD_Y (TOP_Y + TOP_ROWS * ROW_H)
#define LINES_Y (HEAD_Y + LOG_LINE_MIN_H)

_Static_assert(LINES_Y + RECENT_LINES * LOG_LINE_MIN_H <= SETTINGS_CONTENT_H,
               "the recent lines fit under the counters");

typedef enum {
    LROW_PENDING, // "Pending crash 1a2b3c4d" or "Pending: none"
    LROW_DETAIL,  // reason and task
    LROW_WHERE,   // pc and cause
    LROW_BUILD,   // whether the dump's build is this one
    LROW_RESET,   // this boot's reset reason
    LROW_COUNT,
} left_row_t;

_Static_assert((int)LROW_COUNT <= (int)TOP_ROWS, "the report rows fit beside the counters");

static lv_obj_t *s_left[LROW_COUNT];
static lv_obj_t *s_right[DIAG_KINDS];
static lv_obj_t *s_recent[RECENT_LINES];
static char s_left_text[LROW_COUNT][ROW_TEXT_MAX]; // what each row was last given: LONG_DOT edits the label's own text
static char s_right_text[DIAG_KINDS][ROW_TEXT_MAX];
static logring_line_t *s_lines; // the last RECENT_LINES E and W lines, in PSRAM
static size_t s_n;
static uint32_t s_seen_error; // logcap_error_seq() when the lines were read

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:
        return "power on";
    case ESP_RST_EXT:
        return "external pin";
    case ESP_RST_SW:
        return "software restart";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "SDIO";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    case ESP_RST_EFUSE:
        return "eFuse error";
    case ESP_RST_PWR_GLITCH:
        return "power glitch";
    case ESP_RST_CPU_LOCKUP:
        return "CPU lockup";
    default:
        return "unknown";
    }
}

static lv_obj_t *add_row(lv_obj_t *parent, int x, int y, int w)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_recolor(label, false); // reasons and tasks are not ours to trust with markup
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, SLOT24_H); // one line, ending in dots
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, "");
    return label;
}

static void set_left(left_row_t r, const char *text)
{
    ui_set_label(s_left[r], s_left_text[r], ROW_TEXT_MAX, text);
}

static void refresh_report(void)
{
    char text[ROW_TEXT_MAX];
    const proto_report_t *pending = crash_pending();
    proto_report_t r;
    if (pending) {
        r = *pending; // one copy, so the rows agree if the link task acks it meanwhile
        snprintf(text, sizeof(text), "Pending %s report %s", r.kind == PROTO_REPORT_CRASH ? "crash" : "reset", r.id);
        set_left(LROW_PENDING, text);
        if (r.kind == PROTO_REPORT_CRASH) {
            snprintf(text, sizeof(text), "%s in task %s", r.reason, r.task[0] ? r.task : "--");
            set_left(LROW_DETAIL, text);
            snprintf(text, sizeof(text), "pc 0x%08x %s", (unsigned)r.pc, r.cause);
            set_left(LROW_WHERE, text);
            set_left(LROW_BUILD, r.same_build ? "from this build" : "from a different build");
        } else {
            snprintf(text, sizeof(text), "%s, no dump", r.reason);
            set_left(LROW_DETAIL, text);
            set_left(LROW_WHERE, "");
            set_left(LROW_BUILD, "");
        }
    } else {
        set_left(LROW_PENDING, "No pending report");
        set_left(LROW_DETAIL, "");
        set_left(LROW_WHERE, "");
        set_left(LROW_BUILD, "");
    }
    snprintf(text, sizeof(text), "Reset: %s", reset_name(esp_reset_reason()));
    set_left(LROW_RESET, text);
}

static void refresh_counters(void)
{
    for (diag_kind_t k = 0; k < DIAG_KINDS; k++) {
        diag_entry_t e;
        char text[ROW_TEXT_MAX];
        if (!diag_get(k, &e)) {
            continue;
        }
        if (e.count) {
            snprintf(text, sizeof(text), "%s %u: %s", diag_kind_name(k), (unsigned)e.count, e.reason);
        } else {
            snprintf(text, sizeof(text), "%s 0", diag_kind_name(k));
        }
        ui_set_label(s_right[k], s_right_text[k], ROW_TEXT_MAX, text);
    }
}

// Reread the recent lines when any line arrived since the last read. Errors are rare, but the walk is cheap.
static void refresh_recent(void)
{
    const uint32_t err = logcap_error_seq();
    if (err == s_seen_error) {
        return;
    }
    s_seen_error = err;
    s_n = logcap_read_before(logcap_next_seq(), true, s_lines, RECENT_LINES);
    // Newest first would put the latest line where the eye starts, but the ring pages oldest first, so keep that order.
    for (size_t i = 0; i < RECENT_LINES; i++) {
        settings_log_line_set(s_recent[i], i < s_n ? &s_lines[i] : NULL);
    }
    if (s_n == 0) {
        lv_label_set_text_static(s_recent[0], "(no errors or warnings)");
    }
}

static void line_tapped(lv_event_t *e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i < s_n) {
        settings_log_line_open(&s_lines[i]);
    }
}

void tab_errors_tick(void)
{
    if (!s_lines) {
        return;
    }
    refresh_report();
    refresh_counters();
    refresh_recent();
}

void tab_errors_build(lv_obj_t *parent)
{
    s_lines = heap_caps_calloc(RECENT_LINES, sizeof(*s_lines), MALLOC_CAP_SPIRAM);
    if (!s_lines) {
        lv_obj_t *label = lv_label_create(parent);
        lv_label_set_text_static(label, "Out of memory for the error lines");
        lv_obj_set_pos(label, SETTINGS_PAD, SETTINGS_PAD);
        return;
    }
    s_n = 0;
    s_seen_error = UINT32_MAX; // never a real seq, so the first tick reads
    memset(s_left_text, 0, sizeof(s_left_text));
    memset(s_right_text, 0, sizeof(s_right_text));

    for (int r = 0; r < LROW_COUNT; r++) {
        s_left[r] = add_row(parent, LEFT_X, TOP_Y + r * ROW_H, LEFT_W);
    }
    for (int k = 0; k < DIAG_KINDS; k++) {
        s_right[k] = add_row(parent, RIGHT_X, TOP_Y + k * ROW_H, RIGHT_W);
    }

    lv_obj_t *head = lv_label_create(parent);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(head, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_label_set_text_static(head, "Recent errors and warnings (tap a line to read it whole)");
    lv_obj_set_pos(head, SETTINGS_PAD, HEAD_Y);

    for (size_t i = 0; i < RECENT_LINES; i++) {
        s_recent[i] = settings_log_line_create(parent, SETTINGS_PAD, LINES_Y + (int)i * LOG_LINE_MIN_H,
                                               SETTINGS_CONTENT_W - 2 * SETTINGS_PAD, LOG_LINE_MIN_H, line_tapped,
                                               (void *)(intptr_t)i);
    }
    tab_errors_tick();
    ui_log_heap("errors tab built");
}

void tab_errors_leave(void)
{
    settings_log_line_close();
    heap_caps_free(s_lines);
    s_lines = NULL;
    s_n = 0;
    memset(s_left, 0, sizeof(s_left));
    memset(s_right, 0, sizeof(s_right));
    memset(s_recent, 0, sizeof(s_recent));
}
