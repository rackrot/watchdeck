// The Wi-Fi tab: the connection's rows, and an editor that scans, takes a typed network and password on an on-screen
// keyboard, tries the network and saves it to wifi.txt. The work is wifi.c's, on the svc task; this file only starts a
// job and polls its result, so nothing here waits. Everything runs in the LVGL task or under the LVGL lock.
//
// The password is shown in clear only behind the Show button, is never logged, and is kept nowhere but the text area
// and wifi.c's own copies. When the text area is cleared or deleted its buffer is first overwritten with filler of the
// same length (scrub_password). That is best effort: LVGL may have moved the buffer, and freed heap is not wiped.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "kbmap.h"
#include "settings.h"
#include "wifi.h"
#include "wifitxt.h"

#define ROW_TEXT_MAX 128
#define LINE 31 // a row: Montserrat 24's 27 px line and 4 px
#define BTN_W 300
#define BTN_H 56
#define POLL_MS 200
#define LIST_ITEM_H 48 // a scan result: about 10 mm to tap

// The editor, in the content object's coordinates (1024 wide). The keyboard and the scan list share the bottom 264 px,
// where Montserrat 24 keys come out about 50 px tall.
#define KB_H 264
#define KB_Y (SETTINGS_CONTENT_H - KB_H)
#define LABEL_W 130
#define FIELD_X (SETTINGS_PAD + LABEL_W)
#define FIELD_W 600
#define FIELD_H 56
#define SIDE_X (FIELD_X + FIELD_W + SETTINGS_PAD)
#define SIDE_W (SETTINGS_CONTENT_W - SETTINGS_PAD - SIDE_X)
#define ROW1_Y 8
#define ROW2_Y (ROW1_Y + FIELD_H + 8)
#define ROW3_Y (ROW2_Y + FIELD_H + 8)
#define MSG_Y (ROW3_Y + BTN_H + 4)
#define MSG_H 32 // one line; the keyboard starts below it

_Static_assert(MSG_Y + MSG_H <= KB_Y, "the message line fits above the keyboard");

// kbmap.c spells LVGL's symbols and mode labels out so the host tests need no LVGL. They must match.
_Static_assert(__builtin_strcmp(KBMAP_BACKSPACE, LV_SYMBOL_BACKSPACE) == 0, "backspace glyph");
_Static_assert(__builtin_strcmp(KBMAP_KEYBOARD, LV_SYMBOL_KEYBOARD) == 0, "keyboard glyph");
_Static_assert(__builtin_strcmp(KBMAP_LEFT, LV_SYMBOL_LEFT) == 0, "left glyph");
_Static_assert(__builtin_strcmp(KBMAP_RIGHT, LV_SYMBOL_RIGHT) == 0, "right glyph");
_Static_assert(__builtin_strcmp(KBMAP_OK, LV_SYMBOL_OK) == 0, "ok glyph");

static const char *TAG = "tab_wifi";

typedef enum {
    ROW_STATUS,
    ROW_SSID,
    ROW_IP,
    ROW_RSSI,
    ROW_SOURCE,
    ROW_FILE,
    ROW_COUNT,
} row_t;

typedef enum {
    MSG_INFO,
    MSG_GOOD,
    MSG_BAD,
} msg_level_t;

static lv_obj_t *s_content; // the tab's content object, NULL while the tab isn't shown
static bool s_editing;      // the editor is up (and its keyboard may be)

/* ---- The rows ---- */

static lv_obj_t *s_rows[ROW_COUNT];
static char s_row_text[ROW_COUNT][ROW_TEXT_MAX];

static void set_row(row_t r, const char *text)
{
    ui_set_label(s_rows[r], s_row_text[r], ROW_TEXT_MAX, text);
}

void tab_wifi_tick(void)
{
    if (s_editing || !s_rows[0]) {
        return;
    }
    char text[ROW_TEXT_MAX];
    set_row(ROW_STATUS, ui_status_text(STATUS_WIFI));

    char ssid[WIFI_SSID_LEN], shown[WIFI_SSID_LEN];
    wifi_current_ssid(ssid);
    wifiscan_printable(shown, sizeof(shown), ssid);
    snprintf(text, sizeof(text), "SSID %s", shown[0] ? shown : "--");
    set_row(ROW_SSID, text);

    char ip[16];
    snprintf(text, sizeof(text), "IP %s", wifi_ip(ip, sizeof(ip)) ? ip : "--");
    set_row(ROW_IP, text);

    int dbm;
    uint32_t age_ms;
    if (wifi_rssi(&dbm, &age_ms) && age_ms <= 15000) {
        snprintf(text, sizeof(text), "RSSI %d dBm, %u s ago", dbm, (unsigned)(age_ms / 1000));
    } else {
        strlcpy(text, "RSSI --", sizeof(text));
    }
    set_row(ROW_RSSI, text);

    const char *source = wifi_source();
    if (strcmp(source, "sd") == 0) {
        strlcpy(text, "Credentials from wifi.txt on the card", sizeof(text));
    } else if (strcmp(source, "config") == 0) {
        strlcpy(text, "Credentials from the firmware build (Kconfig)", sizeof(text));
    } else if (strcmp(source, "typed") == 0) {
        strlcpy(text, "Credentials typed on this screen, not saved", sizeof(text));
    } else {
        strlcpy(text, "No credentials", sizeof(text));
    }
    set_row(ROW_SOURCE, text);
    set_row(ROW_FILE, wifi_file_present() ? "wifi.txt: found on the card" : "wifi.txt: not found");
}

/* ---- The editor ---- */

static lv_obj_t *s_ssid_ta, *s_pass_ta, *s_show_btn, *s_show_label, *s_scan_btn, *s_connect_btn, *s_save_btn,
    *s_save_label, *s_cancel_btn, *s_msg, *s_list, *s_kb;
static lv_timer_t *s_poll;
static char s_msg_text[ROW_TEXT_MAX];
static char s_ssid_raw[WIFI_SSID_LEN]; // the SSID's bytes when it came from a scan or the current network
static bool s_ssid_exact;              // the SSID field still shows s_ssid_raw (printable only), so use the raw bytes
static bool s_programmatic;            // the SSID field is being set by code, not typed in
static bool s_save_armed;              // "Save without connecting" was tapped once and asks to be tapped again
static bool s_kb_logged;
static bool s_kb_up;                             // the keyboard is shown
static wifiscan_entry_t s_entries[WIFISCAN_MAX]; // what the list shows
static size_t s_entry_n;

// What the editor started and is waiting for.
static wifi_job_kind_t s_pending;
static uint32_t s_pending_serial;

static lv_buttonmatrix_ctrl_t s_ctrl[KBMAP_MODES][KBMAP_MAX_KEYS];

static void set_msg(msg_level_t level, const char *text)
{
    if (!s_msg) {
        return;
    }
    const lv_color_t color = level == MSG_GOOD  ? lv_palette_lighten(LV_PALETTE_GREEN, 1)
                             : level == MSG_BAD ? lv_palette_lighten(LV_PALETTE_RED, 2)
                                                : lv_palette_lighten(LV_PALETTE_GREY, 1);
    lv_obj_set_style_text_color(s_msg, color, 0);
    ui_set_label(s_msg, s_msg_text, sizeof(s_msg_text), text);
}

static const char *editor_ssid(void)
{
    return s_ssid_exact ? s_ssid_raw : lv_textarea_get_text(s_ssid_ta);
}

static void set_ssid_field(const char *raw)
{
    char shown[WIFI_SSID_LEN];
    strlcpy(s_ssid_raw, raw, sizeof(s_ssid_raw));
    s_ssid_exact = true;
    wifiscan_printable(shown, sizeof(shown), raw);
    s_programmatic = true;
    lv_textarea_set_text(s_ssid_ta, shown);
    s_programmatic = false;
}

static void disarm_save(void)
{
    if (s_save_armed) {
        s_save_armed = false;
        lv_label_set_text(s_save_label, "Save without connecting");
    }
}

// Overwrite the password text area's text with same-length filler, then empty it.
static void scrub_password(void)
{
    if (!s_pass_ta) {
        return;
    }
    char filler[WIFITXT_PASS_MAX + 1];
    size_t n = strlen(lv_textarea_get_text(s_pass_ta));
    if (n > WIFITXT_PASS_MAX) {
        n = WIFITXT_PASS_MAX;
    }
    memset(filler, 'x', n);
    filler[n] = '\0';
    lv_textarea_set_text(s_pass_ta, filler);
    lv_textarea_set_text(s_pass_ta, "");
}

// Disabled while a job runs. Cancel goes too: a trial can't be called off from here, and the screen it would return
// to has to stay until the job says how it ended.
static void set_busy(bool busy)
{
    lv_obj_t *const inputs[] = {s_ssid_ta, s_pass_ta, s_scan_btn, s_connect_btn, s_save_btn, s_cancel_btn};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
        if (busy) {
            lv_obj_add_state(inputs[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(inputs[i], LV_STATE_DISABLED);
        }
    }
}

static void show_keyboard(lv_obj_t *ta)
{
    s_kb_up = true;
    lv_keyboard_set_textarea(s_kb, ta);
    lv_obj_set_hidden(s_list, true);
    lv_obj_set_hidden(s_kb, false);
    if (!s_kb_logged) {
        s_kb_logged = true;
        ui_log_heap("wifi editor, keyboard up");
    }
}

static void hide_keyboard(void)
{
    s_kb_up = false;
    lv_keyboard_set_textarea(s_kb, NULL);
    lv_obj_set_hidden(s_kb, true);
    lv_obj_set_hidden(s_list, false);
}

static void field_clicked(lv_event_t *e)
{
    show_keyboard(lv_event_get_user_data(e));
}

// OK ends the SSID and moves on to the password; OK on the password, or the hide key, puts the keyboard away.
static void keyboard_done(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY && lv_keyboard_get_textarea(s_kb) == s_ssid_ta) {
        show_keyboard(s_pass_ta);
    } else {
        hide_keyboard();
    }
}

static void ssid_changed(lv_event_t *e)
{
    (void)e;
    if (!s_programmatic) {
        s_ssid_exact = false; // typed over: the field's text is the name now
    }
    disarm_save();
}

static void pass_changed(lv_event_t *e)
{
    (void)e;
    disarm_save();
}

static void show_toggled(lv_event_t *e)
{
    (void)e;
    const bool shown = lv_obj_has_state(s_show_btn, LV_STATE_CHECKED);
    lv_textarea_set_password_mode(s_pass_ta, !shown);
    lv_label_set_text(s_show_label, shown ? "Hide" : "Show");
}

static void list_item_clicked(lv_event_t *e)
{
    const size_t i = (size_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_entry_n) {
        return;
    }
    set_ssid_field(s_entries[i].ssid);
    scrub_password(); // a password typed for another network stays with that network
    if (s_entries[i].secure) {
        show_keyboard(s_pass_ta);
        set_msg(MSG_INFO, "Type the password.");
    } else {
        set_msg(MSG_INFO, "An open network: no password. Tap Connect and save.");
    }
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    s_entry_n = wifi_scan_results(s_entries, WIFISCAN_MAX);
    if (s_entry_n == 0) {
        lv_label_set_text(lv_label_create(s_list), "No networks listed. Tap Scan, or type a name.");
        return;
    }
    for (size_t i = 0; i < s_entry_n; i++) {
        char shown[WIFI_SSID_LEN], text[WIFI_SSID_LEN + 40];
        wifiscan_printable(shown, sizeof(shown), s_entries[i].ssid);
        snprintf(text, sizeof(text), "%s    %d dBm%s", shown, s_entries[i].rssi, s_entries[i].secure ? "" : "    open");
        lv_obj_t *item = lv_button_create(s_list);
        lv_obj_set_size(item, LV_PCT(100), LIST_ITEM_H);
        lv_obj_add_event_cb(item, list_item_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, text);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

static void start_scan(void)
{
    const esp_err_t err = wifi_scan();
    if (err != ESP_OK) {
        set_msg(MSG_BAD, "Busy. Try the scan again in a moment.");
        return;
    }
    wifi_job_t j;
    wifi_job_get(&j);
    s_pending = WIFI_JOB_SCAN;
    s_pending_serial = j.serial;
    set_busy(true);
    set_msg(MSG_INFO, "Scanning...");
}

static void scan_clicked(lv_event_t *e)
{
    (void)e;
    hide_keyboard();
    start_scan();
}

// Why an input can't go to wifi.txt, or NULL when it can.
static const char *check_input(const char *ssid, const char *pass)
{
    if (ssid[0] == '\0') {
        return "Enter the network name.";
    }
    const size_t n = strlen(pass);
    if (n != 0 && n < 8) {
        return "The password needs 8 to 63 characters.";
    }
    char buf[WIFITXT_FILE_MAX];
    const bool ok = wifitxt_format(ssid, pass, buf) != 0;
    memset(buf, 0, sizeof(buf));
    if (ok) {
        return NULL;
    }
    return n == 64 ? "A 64-character key must be hex digits." : "That name or password can't be used.";
}

static void job_started(wifi_job_kind_t kind, const char *text)
{
    wifi_job_t j;
    wifi_job_get(&j);
    s_pending = kind;
    s_pending_serial = j.serial;
    hide_keyboard();
    set_busy(true);
    set_msg(MSG_INFO, text);
}

static void start_credentials(bool trial)
{
    const char *ssid = editor_ssid();
    const char *pass = lv_textarea_get_text(s_pass_ta);
    const char *why = check_input(ssid, pass);
    if (why) {
        set_msg(MSG_BAD, why);
        return;
    }
    const esp_err_t err = trial ? wifi_try(ssid, pass) : wifi_save(ssid, pass);
    if (err == ESP_OK) {
        char shown[WIFI_SSID_LEN], text[ROW_TEXT_MAX];
        wifiscan_printable(shown, sizeof(shown), ssid);
        snprintf(text, sizeof(text), trial ? "Trying %s (up to 20 s)..." : "Saving...", shown);
        job_started(trial ? WIFI_JOB_TRIAL : WIFI_JOB_SAVE, text);
    } else if (err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_STATE) {
        set_msg(MSG_BAD, "Busy. Try again in a moment.");
    } else {
        set_msg(MSG_BAD, "That name or password can't be used.");
    }
}

static void connect_clicked(lv_event_t *e)
{
    (void)e;
    disarm_save();
    start_credentials(true);
}

static void save_clicked(lv_event_t *e)
{
    (void)e;
    if (!s_save_armed) {
        if (editor_ssid()[0] == '\0') {
            set_msg(MSG_BAD, "Enter the network name.");
            return;
        }
        s_save_armed = true;
        lv_label_set_text(s_save_label, "Tap again to save");
        set_msg(MSG_INFO, "Saves for the next boot without trying it. Tap again to confirm.");
        return;
    }
    disarm_save();
    start_credentials(false);
}

static const char *save_text(wifi_save_t save)
{
    switch (save) {
    case WIFI_SAVE_OK:
        return "saved to wifi.txt";
    case WIFI_SAVE_NO_CARD:
        return "no card in the slot";
    case WIFI_SAVE_SHARED:
        return "the card is shared over USB, or read-only";
    case WIFI_SAVE_TAINTED:
        return "a card error earlier; restart the board";
    case WIFI_SAVE_DIR_FULL:
        return "the card's root folder is full";
    case WIFI_SAVE_BUSY:
        return "the board was busy";
    default:
        return "a card write error";
    }
}

static void finished(const wifi_job_t *j)
{
    char text[ROW_TEXT_MAX];
    switch (j->kind) {
    case WIFI_JOB_SCAN:
        if (j->ok) {
            fill_list();
            snprintf(text, sizeof(text), s_entry_n ? "%u network(s). Tap one, or type a name." : "No networks found.",
                     (unsigned)s_entry_n);
            set_msg(MSG_INFO, text);
        } else {
            snprintf(text, sizeof(text), "Scan failed: %s", esp_err_to_name(j->err));
            set_msg(MSG_BAD, text);
        }
        break;
    case WIFI_JOB_TRIAL:
        if (j->ok) {
            scrub_password(); // joined: the password has done its work
            if (j->save == WIFI_SAVE_OK) {
                set_msg(MSG_GOOD, "Connected and saved to wifi.txt.");
            } else {
                snprintf(text, sizeof(text), "Connected, but not saved: %s.", save_text(j->save));
                set_msg(MSG_BAD, text);
            }
        } else {
            char old[WIFI_SSID_LEN], shown[WIFI_SSID_LEN];
            wifi_current_ssid(old);
            wifiscan_printable(shown, sizeof(shown), old);
            if (shown[0]) {
                snprintf(text, sizeof(text), "Could not join: %s. Back on %s.", wifistate_fail_text(j->fail), shown);
            } else {
                snprintf(text, sizeof(text), "Could not join: %s.", wifistate_fail_text(j->fail));
            }
            set_msg(MSG_BAD, text);
        }
        break;
    case WIFI_JOB_SAVE:
        if (j->ok) {
            set_msg(MSG_GOOD, "Saved to wifi.txt. It applies at the next boot.");
        } else {
            snprintf(text, sizeof(text), "Not saved: %s.", save_text(j->save));
            set_msg(MSG_BAD, text);
        }
        break;
    case WIFI_JOB_NONE:
        break;
    }
}

// Follows the job the editor started, every POLL_MS.
static void editor_poll(lv_timer_t *timer)
{
    (void)timer;
    if (s_pending == WIFI_JOB_NONE) {
        return;
    }
    wifi_job_t j;
    wifi_job_get(&j);
    if (j.serial != s_pending_serial) {
        s_pending = WIFI_JOB_NONE; // another job took over (the user left and came back)
        set_busy(false);
        return;
    }
    if (j.running) {
        if (s_pending == WIFI_JOB_TRIAL && j.joined) {
            set_msg(MSG_INFO, "Joined. Saving wifi.txt...");
        }
        return;
    }
    s_pending = WIFI_JOB_NONE;
    set_busy(false);
    finished(&j);
}

static void build_rows(void);

static void editor_close(void)
{
    scrub_password(); // before the text area goes, whichever way it goes
    if (s_poll) {
        lv_timer_delete(s_poll);
        s_poll = NULL;
    }
    s_editing = false;
    s_kb_up = false;
    s_pending = WIFI_JOB_NONE;
    memset(s_ssid_raw, 0, sizeof(s_ssid_raw));
    s_ssid_exact = false;
    s_save_armed = false;
    s_ssid_ta = s_pass_ta = s_show_btn = s_show_label = s_scan_btn = s_connect_btn = s_save_btn = s_save_label = NULL;
    s_cancel_btn = s_msg = s_list = s_kb = NULL;
    memset(s_msg_text, 0, sizeof(s_msg_text));
}

static void cancel_async(void *unused)
{
    (void)unused;
    if (!s_content || !s_editing) {
        return;
    }
    editor_close(); // scrubs the password, then the clean deletes its text area
    lv_obj_clean(s_content);
    settings_set_swipe_close(true);
    build_rows();
}

static void cancel_clicked(lv_event_t *e)
{
    (void)e;
    lv_async_call(cancel_async, NULL); // not from inside the button's own event
}

static lv_obj_t *add_label(const char *text, int y)
{
    lv_obj_t *label = lv_label_create(s_content);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, SETTINGS_PAD, y + (FIELD_H - SLOT24_H) / 2);
    return label;
}

static lv_obj_t *add_field(int y, uint32_t max_len)
{
    lv_obj_t *ta = lv_textarea_create(s_content);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_obj_set_size(ta, FIELD_W, FIELD_H);
    lv_obj_set_pos(ta, FIELD_X, y);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_32, 0);
    lv_obj_add_event_cb(ta, field_clicked, LV_EVENT_CLICKED, ta);
    return ta;
}

static lv_obj_t *add_button(const char *text, int x, int y, int w, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(s_content);
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_pos(btn, x, y);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (label_out) {
        *label_out = label;
    }
    return btn;
}

// Hand the three modes the maps from kbmap.c. lv_keyboard_set_map keeps the pointers, and the default handler switches
// modes from LVGL's own table, so this writes to that.
static void install_keyboard_maps(void)
{
    static const lv_keyboard_mode_t MODES[KBMAP_MODES] = {LV_KEYBOARD_MODE_TEXT_LOWER, LV_KEYBOARD_MODE_TEXT_UPPER,
                                                          LV_KEYBOARD_MODE_SPECIAL};
    for (int m = 0; m < KBMAP_MODES; m++) {
        const uint8_t *keys = kbmap_keys((kbmap_mode_t)m);
        const size_t n = kbmap_key_count((kbmap_mode_t)m);
        for (size_t k = 0; k < n; k++) {
            lv_buttonmatrix_ctrl_t c = keys[k] & KBKEY_WIDTH_MASK;
            if (keys[k] & KBKEY_MODE) {
                c |= LV_KEYBOARD_CTRL_BUTTON_FLAGS;
            } else if (keys[k] & KBKEY_CHECKED) {
                c |= LV_BUTTONMATRIX_CTRL_CHECKED;
            }
            s_ctrl[m][k] = c;
        }
        lv_keyboard_set_map(s_kb, MODES[m], kbmap_text((kbmap_mode_t)m), s_ctrl[m]);
    }
    lv_keyboard_set_mode(s_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static void build_editor(void)
{
    s_editing = true;
    s_kb_logged = false;
    settings_set_swipe_close(false); // the keyboard and the list take drags

    add_label("Network", ROW1_Y);
    s_ssid_ta = add_field(ROW1_Y, WIFITXT_SSID_MAX);
    lv_obj_add_event_cb(s_ssid_ta, ssid_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_scan_btn = add_button("Scan", SIDE_X, ROW1_Y, SIDE_W, scan_clicked, NULL);

    add_label("Password", ROW2_Y);
    s_pass_ta = add_field(ROW2_Y, WIFITXT_PASS_MAX);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_password_show_time(s_pass_ta, 0); // not even the last character in clear
    lv_obj_add_event_cb(s_pass_ta, pass_changed, LV_EVENT_VALUE_CHANGED, NULL);
    s_show_btn = add_button("Show", SIDE_X, ROW2_Y, SIDE_W, NULL, &s_show_label);
    lv_obj_set_checkable(s_show_btn, true);
    lv_obj_add_event_cb(s_show_btn, show_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    s_connect_btn = add_button("Connect and save", SETTINGS_PAD, ROW3_Y, 320, connect_clicked, NULL);
    s_save_btn =
        add_button("Save without connecting", SETTINGS_PAD + 320 + 12, ROW3_Y, 400, save_clicked, &s_save_label);
    s_cancel_btn =
        add_button("Cancel", SETTINGS_PAD + 320 + 12 + 400 + 12, ROW3_Y,
                   SETTINGS_CONTENT_W - SETTINGS_PAD - (SETTINGS_PAD + 320 + 12 + 400 + 12), cancel_clicked, NULL);

    s_msg = lv_label_create(s_content);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_DOT);
    lv_label_set_recolor(s_msg, false);
    lv_obj_set_size(s_msg, SETTINGS_CONTENT_W - 2 * SETTINGS_PAD, MSG_H);
    lv_obj_set_pos(s_msg, SETTINGS_PAD, MSG_Y);
    lv_label_set_text(s_msg, "");
    memset(s_msg_text, 0, sizeof(s_msg_text));

    // The scan results: a column of buttons that scrolls. A plain object, since lv_list is deprecated.
    s_list = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, SETTINGS_CONTENT_W, KB_H);
    lv_obj_set_pos(s_list, 0, KB_Y);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_style_pad_hor(s_list, SETTINGS_PAD, 0);

    s_kb = lv_keyboard_create(s_content);
    lv_obj_set_size(s_kb, SETTINGS_CONTENT_W, KB_H);
    lv_obj_set_pos(s_kb, 0, KB_Y);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_24, LV_PART_ITEMS);
    install_keyboard_maps();
    lv_obj_add_event_cb(s_kb, keyboard_done, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, keyboard_done, LV_EVENT_CANCEL, NULL);
    lv_obj_set_hidden(s_kb, true);

    // Start from the network the board is on: the common reason to be here is a changed password.
    char current[WIFI_SSID_LEN];
    wifi_current_ssid(current);
    set_ssid_field(current);
    fill_list();
    s_poll = lv_timer_create(editor_poll, POLL_MS, NULL);
    ui_log_heap("wifi editor built");
    start_scan();
}

/* ---- The rows view ---- */

static void change_async(void *unused)
{
    (void)unused;
    if (!s_content || s_editing) {
        return;
    }
    lv_obj_clean(s_content);
    memset(s_rows, 0, sizeof(s_rows));
    build_editor();
}

static void change_clicked(lv_event_t *e)
{
    (void)e;
    lv_async_call(change_async, NULL);
}

static void build_rows(void)
{
    memset(s_row_text, 0, sizeof(s_row_text));
    for (row_t r = 0; r < ROW_COUNT; r++) {
        lv_obj_t *label = lv_label_create(s_content);
        s_rows[r] = label;
        lv_obj_set_width(label, SETTINGS_CONTENT_W - 2 * SETTINGS_PAD);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, SLOT24_H);
        lv_obj_set_pos(label, SETTINGS_PAD, SETTINGS_PAD + (int)r * LINE);
        lv_label_set_text(label, "");
    }
    lv_obj_t *btn = add_button("Change Wi-Fi", SETTINGS_PAD, SETTINGS_PAD + ROW_COUNT * LINE + SETTINGS_PAD, BTN_W,
                               change_clicked, NULL);
    (void)btn;
    tab_wifi_tick();
}

void tab_wifi_build(lv_obj_t *parent)
{
    s_content = parent;
    s_editing = false;
    build_rows();
}

void tab_wifi_leave(void)
{
    if (s_editing) {
        ESP_LOGI(TAG, "left the editor");
    }
    editor_close();
    memset(s_rows, 0, sizeof(s_rows));
    s_content = NULL;
}

bool tab_wifi_editing(void)
{
    // An editor with nothing typed, no job and no keyboard is just a view, and idles out like the others.
    return s_editing && (s_pending != WIFI_JOB_NONE || s_kb_up || (s_pass_ta && lv_textarea_get_text(s_pass_ta)[0]));
}

bool tab_wifi_busy(void)
{
    return s_editing && (s_pending == WIFI_JOB_TRIAL || s_pending == WIFI_JOB_SAVE);
}
