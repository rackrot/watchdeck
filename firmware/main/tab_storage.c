// The Storage tab: the TF card (what it is, who has it), what the board keeps on it (wifi.txt and the sleep files), a
// free-space count, and the share over USB. The card reads and the count run on the svc task (svc.h); this file starts
// a job and polls for its result from the tab's once-a-second tick, so nothing here waits and a job that outlives the
// tab only writes to statics nobody is looking at. Everything else runs in the LVGL task or under the LVGL lock.
//
// The tab never writes to the card. The share is usb.c's (it unmounts the card and maps the raw disk to USB), and
// Stop sharing restarts the board. Text from the card (the SSID, file names) goes to labels with recolor off.

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cardfs.h"
#include "esp_log.h"
#include "sdcard.h"
#include "settings.h"
#include "storagefmt.h"
#include "svc.h"
#include "usb.h"

#define ROW_TEXT_MAX 128
#define LINE 31 // a row: Montserrat 24's 27 px line and 4 px
#define BTN_H 56
#define SHARE_W 360
#define FREE_W 300
#define PROBE_TICKS 5    // wifi.txt and the sleep files are read every 5th tick, and when the owner changes
#define BACKOFF_TICKS 30 // after a probe that failed, the next waits this many ticks
#define WIFI_READ_MAX 256
#define ARMED_TICKS 10   // a first tap on Share or Stop sharing is forgotten after this many seconds
#define ARM_GUARD_MS 600 // a second tap sooner than this after the first is a double tap, not a confirmation
#define MSG_TEXT_MAX 192

#define SHARE_WARNING                                                                                                  \
    "The device can't save the sleep screen or Wi-Fi while the card is shared. To take it back, the board restarts."
#define EJECT_WARNING                                                                                                  \
    "Eject the drive on your computer first. The board restarts to take the card back (about 8 s); the pairing stays."
#define FREE_RUNNING "Counting free space... this can take 10 s on a large card."

static const char *TAG = "tab_storage";

typedef enum {
    ROW_CARD,
    ROW_OWNER,
    ROW_WIFI,
    ROW_SLEEP,
    ROW_FREE,
    ROW_COUNT,
} row_t;

typedef enum {
    MSG_INFO,
    MSG_GOOD,
    MSG_BAD,
} msg_level_t;

/* ---- The jobs ---- */

// A job's result is published by its state: IDLE, RUNNING (the job owns the result), DONE (the tab owns it until it
// takes it and sets IDLE).
enum { JOB_IDLE, JOB_RUNNING, JOB_DONE };

typedef struct {
    storagefmt_access_t wifi_access, sleep_access;
    storagefmt_wifi_t wifi;
    bool failed; // a read failed (not "absent" or "detached")
    char ssid[WIFITXT_SSID_MAX + 1];
    bool cfg;
    size_t images;
    char img_name[STORAGEFMT_IMG_NAME];
    uint64_t img_size;
} probe_t;

static probe_t s_probe;
static _Atomic int s_probe_state;

typedef struct {
    esp_err_t err;
    uint64_t total, free_bytes;
} space_t;

static space_t s_space;
static _Atomic int s_space_state;

// Why the card can't be read now. A device-owned card that says so is between mounts.
static storagefmt_access_t unavailable(void)
{
    switch (sdcard_owner()) {
    case SDCARD_SHARED:
        return STORAGEFMT_SHARED;
    case SDCARD_NO_CARD:
        return STORAGEFMT_NO_CARD;
    default:
        return STORAGEFMT_ERROR;
    }
}

// What a cardfs result says about reading. A real failure is remembered in *failure for the caller to report once.
static storagefmt_access_t access_of(esp_err_t err, esp_err_t *failure)
{
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        return STORAGEFMT_READ;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return unavailable();
    }
    if (*failure == ESP_OK) {
        *failure = err;
    }
    return STORAGEFMT_ERROR;
}

// The entries of watchdeck/ that matter here. FAT folds case, so a card edited on a PC may spell them in capitals.
static void list_cb(const char *name, size_t size, bool is_dir, void *ctx)
{
    probe_t *p = ctx;
    const size_t n = strlen(name);
    if (is_dir) {
        return;
    }
    if (strcasecmp(name, "sleep.cfg") == 0) {
        p->cfg = true;
    } else if (n > 4 && strcasecmp(name + n - 4, ".img") == 0 && p->images++ == 0) {
        strlcpy(p->img_name, name, sizeof(p->img_name));
        p->img_size = size;
    }
}

static void probe_job(void *arg)
{
    probe_t p = {0};
    esp_err_t failure = ESP_OK;
    // The boot parse reads two lines and ignores the rest of the file, so read well past a longest valid file (100
    // bytes with CRLF) and parse what fits: a file with more lines after the password is still valid.
    char buf[WIFI_READ_MAX];
    size_t len = 0;
    esp_err_t err = cardfs_read("wifi.txt", buf, sizeof(buf), &len);
    p.wifi_access = access_of(err == ESP_ERR_INVALID_SIZE ? ESP_OK : err, &failure); // INVALID_SIZE: it's there
    if (err == ESP_OK || err == ESP_ERR_INVALID_SIZE) {
        p.wifi = storagefmt_wifi_parse(buf, len, p.ssid);
    } else {
        p.wifi = STORAGEFMT_WIFI_ABSENT;
    }
    memset(buf, 0, sizeof(buf)); // the password was in it

    err = cardfs_list(list_cb, &p);
    p.sleep_access = access_of(err, &failure);
    if (failure != ESP_OK) {
        p.failed = true;
        sdcard_note_failure("storage tab read", failure); // logs, counts, remounts a tainted card; once per probe
    }

    s_probe = p;
    atomic_store(&s_probe_state, JOB_DONE);
}

static void space_job(void *arg)
{
    space_t s = {0};
    s.err = cardfs_free_space(&s.total, &s.free_bytes);
    if (s.err != ESP_OK && s.err != ESP_ERR_INVALID_STATE) {
        sdcard_note_failure("free space", s.err);
    }
    s_space = s;
    atomic_store(&s_space_state, JOB_DONE);
}

// Queue a job unless its state says one is out. Returns whether it was queued.
static bool start_job(_Atomic int *state, svc_job_t job)
{
    int idle = JOB_IDLE;
    if (!atomic_compare_exchange_strong(state, &idle, JOB_RUNNING)) {
        return false;
    }
    if (svc_post(job, NULL) != ESP_OK) {
        atomic_store(state, JOB_IDLE);
        return false;
    }
    return true;
}

// The state is DONE: take the result into `out` and free the job's slot.
static bool take_job(_Atomic int *state, void *out, const void *result, size_t size)
{
    if (atomic_load(state) != JOB_DONE) {
        return false;
    }
    memcpy(out, result, size);
    atomic_store(state, JOB_IDLE);
    return true;
}

/* ---- The tab ---- */

static lv_obj_t *s_content; // the tab's content object, NULL while the tab isn't shown
static lv_obj_t *s_rows[ROW_COUNT];
static lv_obj_t *s_main_btn, *s_main_label, *s_free_btn, *s_msg;
static char s_row_text[ROW_COUNT][ROW_TEXT_MAX];
static char s_msg_text[MSG_TEXT_MAX];
static const char *s_main_text; // what the main button's label shows now; the literals below, compared by address

static probe_t s_seen;       // the last probe taken; wifi_access and sleep_access say whether it has been
static space_t s_space_seen; // the last free-space count, with s_space_known
static bool s_space_known;
static sdcard_owner_t s_owner; // the owner at the last tick, to catch a change
static unsigned s_probe_wait;  // ticks until the next probe; 0 starts one as soon as none is out
static bool s_armed;           // Share or Stop sharing was tapped once and asks to be tapped again
static unsigned s_armed_ticks;
static uint32_t s_armed_at; // lv_tick_get() of that first tap
static bool s_sharing;      // a share was queued from here and has not ended

static void set_row(row_t r, const char *text)
{
    ui_set_label(s_rows[r], s_row_text[r], ROW_TEXT_MAX, text);
}

// Shows s_msg_text in the colour of `level`. The label points at the buffer, so the text costs the pool nothing.
static void show_msg(msg_level_t level)
{
    lv_obj_set_style_text_color(s_msg,
                                level == MSG_GOOD  ? lv_palette_lighten(LV_PALETTE_GREEN, 1)
                                : level == MSG_BAD ? lv_palette_lighten(LV_PALETTE_RED, 2)
                                                   : lv_palette_lighten(LV_PALETTE_GREY, 1),
                                0);
    lv_label_set_text_static(s_msg, s_msg_text);
}

static void set_msg(msg_level_t level, const char *text)
{
    strlcpy(s_msg_text, text, sizeof(s_msg_text));
    show_msg(level);
}

static void set_msgf(msg_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_msgf(msg_level_t level, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_msg_text, sizeof(s_msg_text), fmt, args);
    va_end(args);
    show_msg(level);
}

static void set_enabled(lv_obj_t *obj, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
}

static void disarm(void)
{
    if (s_armed) {
        s_armed = false;
        set_msg(MSG_INFO, "");
    }
}

// What the main button says and whether it can be tapped, from the owner, a share under way and the first tap.
static void refresh_buttons(void)
{
    const sdcard_owner_t owner = sdcard_owner();
    const char *text = "Share over USB";
    bool enabled = owner == SDCARD_DEVICE && usb_ready();
    if (s_sharing || usb_share_busy()) {
        text = "Sharing...";
        enabled = false;
    } else if (owner == SDCARD_SHARED) {
        text = s_armed ? "I ejected it, restart" : "Stop sharing";
        enabled = true;
    } else if (s_armed) {
        text = "Tap again to share";
    }
    if (text != s_main_text) { // literals, so equal text is the same pointer
        s_main_text = text;
        lv_label_set_text_static(s_main_label, text);
    }
    set_enabled(s_main_btn, enabled);
    set_enabled(s_free_btn, owner == SDCARD_DEVICE && atomic_load(&s_space_state) == JOB_IDLE);
}

static void render(void)
{
    const sdcard_owner_t owner = sdcard_owner();
    sdcard_info_t info;
    sdcard_info(&info);
    char text[ROW_TEXT_MAX];

    storagefmt_card(text, sizeof(text), info.present, info.name, info.bytes, info.fs,
                    owner == SDCARD_DEVICE && !cardfs_writable());
    set_row(ROW_CARD, text);
    storagefmt_owner(text, sizeof(text),
                     owner == SDCARD_DEVICE   ? STORAGEFMT_OWNER_DEVICE
                     : owner == SDCARD_SHARED ? STORAGEFMT_OWNER_SHARED
                                              : STORAGEFMT_OWNER_NONE,
                     info.present, usb_connected(), info.error == ESP_OK ? "" : esp_err_to_name(info.error));
    set_row(ROW_OWNER, text);

    // The files are only known while the device has the card; the probe's answer from before a share doesn't stand.
    const bool mine = owner == SDCARD_DEVICE;
    storagefmt_wifi(text, sizeof(text), mine ? s_seen.wifi_access : unavailable(), s_seen.wifi, s_seen.ssid);
    set_row(ROW_WIFI, text);
    storagefmt_sleep(text, sizeof(text), mine ? s_seen.sleep_access : unavailable(), s_seen.cfg, s_seen.images,
                     s_seen.img_name, s_seen.img_size);
    set_row(ROW_SLEEP, text);
    storagefmt_free(text, sizeof(text), s_space_known && mine, s_space_seen.free_bytes, s_space_seen.total);
    set_row(ROW_FREE, text);
    refresh_buttons();
}

static void share_ended(void)
{
    s_sharing = false;
    if (sdcard_owner() == SDCARD_SHARED) {
        set_msg(MSG_GOOD, "The card is shared. Eject the drive on your computer before you stop sharing.");
    } else if (usb_error() == ESP_OK) {
        set_msg(MSG_BAD, "The card changed hands; try again.");
    } else {
        set_msgf(MSG_BAD, "Sharing failed: %s.", esp_err_to_name(usb_error()));
    }
}

static void space_taken(void)
{
    disarm(); // the result replaces the message, so a share warning must not stay armed behind it
    if (s_space_seen.err == ESP_OK) {
        s_space_known = true;
        set_msg(MSG_INFO, "");
        return;
    }
    s_space_known = false;
    if (s_space_seen.err == ESP_ERR_INVALID_STATE) {
        set_msg(MSG_BAD, "The card isn't available now.");
    } else {
        set_msgf(MSG_BAD, "Could not count the free space: %s.", esp_err_to_name(s_space_seen.err));
    }
}

void tab_storage_tick(void)
{
    if (!s_content) {
        return;
    }
    const sdcard_owner_t owner = sdcard_owner();
    if (owner != s_owner) { // the card changed hands: look again at once, and drop what was known
        s_owner = owner;
        s_probe_wait = 0;
        memset(&s_seen, 0, sizeof(s_seen));
        s_space_known = false;
        disarm();
    }
    if (owner == SDCARD_DEVICE) {
        if (s_probe_wait > 0) {
            s_probe_wait--;
        }
        if (s_probe_wait == 0 && start_job(&s_probe_state, probe_job)) {
            s_probe_wait = PROBE_TICKS;
        }
    }
    if (take_job(&s_probe_state, &s_seen, &s_probe, sizeof(s_seen)) && s_seen.failed) {
        s_probe_wait = BACKOFF_TICKS; // don't hammer a card that just failed
    }
    if (take_job(&s_space_state, &s_space_seen, &s_space, sizeof(s_space_seen))) {
        space_taken();
    }
    if (s_sharing && !usb_share_busy()) {
        share_ended();
    }
    if (s_armed && ++s_armed_ticks >= ARMED_TICKS) {
        disarm();
    }
    render();
}

static void main_clicked(lv_event_t *e)
{
    (void)e;
    if (s_armed && lv_tick_elaps(s_armed_at) < ARM_GUARD_MS) {
        return; // a double tap isn't a confirmation: the person hasn't had time to read
    }
    if (!s_armed) { // the first tap says what will happen, the second does it
        s_armed = true;
        s_armed_ticks = 0;
        s_armed_at = lv_tick_get();
        switch (sdcard_owner()) {
        case SDCARD_DEVICE:
            set_msg(MSG_INFO, SHARE_WARNING);
            break;
        case SDCARD_SHARED:
            set_msg(MSG_INFO, EJECT_WARNING);
            break;
        default:
            s_armed = false;
            break;
        }
        refresh_buttons();
        return;
    }
    s_armed = false;
    if (sdcard_owner() == SDCARD_SHARED) {
        usb_stop_sharing(); // restarts the board
        return;
    }
    const esp_err_t err = usb_share();
    if (err == ESP_OK) {
        s_sharing = true;
        set_msg(MSG_INFO, "Sharing the card...");
    } else if (err == ESP_ERR_NO_MEM) {
        set_msg(MSG_BAD, "Busy. Try again in a moment.");
    } else {
        set_msgf(MSG_BAD, "The card can't be shared now (%s).", ui_status_text(STATUS_USB));
    }
    refresh_buttons();
}

static void free_clicked(lv_event_t *e)
{
    (void)e;
    disarm();
    if (start_job(&s_space_state, space_job)) {
        set_msg(MSG_INFO, FREE_RUNNING);
    } else {
        set_msg(MSG_BAD, "Busy. Try again in a moment.");
    }
    refresh_buttons();
}

static lv_obj_t *add_button(const char *text, int x, int y, int w, lv_event_cb_t cb, lv_obj_t **label_out)
{
    lv_obj_t *btn = lv_button_create(s_content);
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_pos(btn, x, y);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_static(label, text);
    lv_obj_center(label);
    if (label_out) {
        *label_out = label;
    }
    return btn;
}

void tab_storage_build(lv_obj_t *parent)
{
    s_content = parent;
    memset(s_row_text, 0, sizeof(s_row_text));
    memset(&s_seen, 0, sizeof(s_seen));
    memset(&s_space_seen, 0, sizeof(s_space_seen));
    s_space_known = false;
    s_armed = s_sharing = false;
    s_main_text = NULL;
    s_owner = (sdcard_owner_t)-1; // the first tick sees a change and reads the card
    s_probe_wait = 0;
    s_sharing = usb_share_busy(); // a share started before the tab was left and is still under way

    int y = SETTINGS_PAD;
    for (row_t r = 0; r < ROW_COUNT; r++, y += LINE) {
        lv_obj_t *label = lv_label_create(parent);
        s_rows[r] = label;
        lv_label_set_recolor(label, false); // an SSID or a file name can hold '#'
        lv_obj_set_width(label, SETTINGS_CONTENT_W - 2 * SETTINGS_PAD);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, SLOT24_H);
        lv_obj_set_pos(label, SETTINGS_PAD, y);
        lv_label_set_text(label, "");
    }
    y += SETTINGS_PAD;
    s_main_btn = add_button("Share over USB", SETTINGS_PAD, y, SHARE_W, main_clicked, &s_main_label);
    s_free_btn = add_button("Check free space", SETTINGS_PAD + SHARE_W + SETTINGS_PAD, y, FREE_W, free_clicked, NULL);
    y += BTN_H + SETTINGS_PAD;

    s_msg = lv_label_create(parent);
    lv_label_set_recolor(s_msg, false);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_msg, SETTINGS_CONTENT_W - 2 * SETTINGS_PAD);
    lv_obj_set_pos(s_msg, SETTINGS_PAD, y);
    set_msg(MSG_INFO, atomic_load(&s_space_state) == JOB_RUNNING ? FREE_RUNNING : "");
    if (!usb_ready()) {
        set_msgf(MSG_BAD, "USB didn't start (%s), so the card can't be shared.", esp_err_to_name(usb_error()));
    }

    // A probe that finished while the tab was away is older than this build; throw it away and read again.
    int done = JOB_DONE;
    atomic_compare_exchange_strong(&s_probe_state, &done, JOB_IDLE);
    tab_storage_tick();
    ui_log_heap("storage tab built");
}

void tab_storage_leave(void)
{
    memset(s_rows, 0, sizeof(s_rows));
    s_main_btn = s_main_label = s_free_btn = s_msg = NULL;
    s_content = NULL;
    if (s_armed) {
        ESP_LOGI(TAG, "left with a share not confirmed");
    }
    s_armed = false;
}
