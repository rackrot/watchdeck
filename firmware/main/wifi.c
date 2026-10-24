#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "cardfs.h"
#include "diag.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logcap.h"
#include "mdns.h"
#include "sdcard.h"
#include "svc.h"
#include "wifitxt.h"

#define RECONNECT_DELAY_US (3 * 1000 * 1000)
#define TRIAL_TIMEOUT_US (20 * 1000 * 1000)
#define RSSI_REFRESH_US (5 * 1000 * 1000)
#define SCAN_RECORDS 20 // what one scan reads from the driver, before de-duplication
#define NOTE_MAX 96
#define HOSTNAME "watchdeck"
#define WIFI_FILE SDCARD_MOUNT_POINT "/wifi.txt"

static const char *TAG = "wifi";

static wifi_status_cb_t s_status_cb;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_trial_timer;
static esp_timer_handle_t s_rssi_timer;

// One mutex serializes the machine and every connect: the event handler, the two timers and the svc job that begins a
// trial all take it. It is held across the driver calls and never across a status callback or the LVGL lock, and no
// UI call takes it (the UI reads the snapshots below).
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static wifi_machine_t s_m;
static char s_cur_ssid[WIFI_SSID_LEN]; // the network the board is on or retries
static char s_cur_pass[WIFITXT_PASS_MAX + 1];
static char s_try_ssid[WIFI_SSID_LEN]; // the trial's candidate, from begin to end of the trial
static char s_try_pass[WIFITXT_PASS_MAX + 1];
// The credentials of the job that holds the claim, from wifi_try or wifi_save to the svc job that takes them.
static struct {
    char ssid[WIFI_SSID_LEN];
    char pass[WIFITXT_PASS_MAX + 1];
} s_req;

// What the UI and the other tasks read, behind a spinlock held for a copy only.
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_job_t s_job;
static char s_snap_ssid[WIFI_SSID_LEN];
static const char *s_source = "none";
static bool s_file_present;
static wifiscan_entry_t s_scan[WIFISCAN_MAX];
static size_t s_scan_n;
static bool s_rssi_valid;
static int s_rssi_dbm;
static int64_t s_rssi_at_us;

static volatile wifi_conn_state_t s_state_mirror; // s_m.state, for readers that can't take s_lock
static volatile bool s_scanning;
static volatile bool s_rssi_queued; // one RSSI job at a time in svc's queue
static bool s_rssi_timed;           // the first refresh's duration has been logged

static void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void report(const char *fmt, ...)
{
    char text[NOTE_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "%s", text);
    if (s_status_cb) {
        s_status_cb(text);
    }
}

#define SSID_MAX WIFITXT_SSID_MAX
#define PASS_MAX WIFITXT_PASS_MAX

// Read SSID and password from the card. Returns false when the file is absent or invalid. Only lengths are logged.
static bool read_card_credentials(char ssid[SSID_MAX + 1], char pass[PASS_MAX + 1])
{
    FILE *f = fopen(WIFI_FILE, "r");
    if (!f) {
        ESP_LOGI(TAG, "no %s, using Kconfig credentials", WIFI_FILE);
        return false;
    }
    const wifitxt_result_t res = wifitxt_parse(f, ssid, pass);
    fclose(f);
    switch (res) {
    case WIFITXT_OK:
        return true;
    case WIFITXT_EMPTY:
        ESP_LOGW(TAG, "%s is empty", WIFI_FILE);
        break;
    case WIFITXT_SSID_LONG:
        ESP_LOGE(TAG, "SSID in %s is longer than %u chars", WIFI_FILE, SSID_MAX);
        break;
    case WIFITXT_PASS_LONG:
        ESP_LOGE(TAG, "password in %s is longer than %u chars", WIFI_FILE, PASS_MAX);
        break;
    }
    return false;
}

// Set the DHCP hostname and answer as HOSTNAME.local. Failures are logged and boot continues.
static void start_mdns(esp_netif_t *sta)
{
    // mdns binds to the netif registered as "WIFI_STA_DEF" (its predefined STA interface).
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") != sta) {
        ESP_LOGE(TAG, "STA netif is not WIFI_STA_DEF, mDNS will not bind to it");
    }
    esp_err_t err = esp_netif_set_hostname(sta, HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_hostname: %s", esp_err_to_name(err));
    }
    err = mdns_init();
    if (err == ESP_OK) {
        err = mdns_hostname_set(HOSTNAME);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS setup: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "mDNS hostname %s.local", HOSTNAME);
    }
}

/* ---- The job snapshot ---- */

// Claim the one job slot. False while another job runs.
static bool job_claim(wifi_job_kind_t kind)
{
    bool ok;
    taskENTER_CRITICAL(&s_snap_mux);
    ok = !s_job.running;
    if (ok) {
        s_job = (wifi_job_t){.kind = kind, .running = true, .serial = s_job.serial + 1};
    }
    taskEXIT_CRITICAL(&s_snap_mux);
    return ok;
}

static void job_done(bool ok, wifi_fail_t fail, wifi_save_t save, esp_err_t err)
{
    taskENTER_CRITICAL(&s_snap_mux);
    s_job.running = false;
    s_job.ok = ok;
    s_job.fail = fail;
    s_job.save = save;
    s_job.err = err;
    taskEXIT_CRITICAL(&s_snap_mux);
}

static void job_joined(const char *ssid)
{
    taskENTER_CRITICAL(&s_snap_mux);
    s_job.joined = true;
    s_source = "typed";
    strlcpy(s_snap_ssid, ssid, sizeof(s_snap_ssid));
    taskEXIT_CRITICAL(&s_snap_mux);
}

static void snapshot_network(const char *ssid, const char *source)
{
    taskENTER_CRITICAL(&s_snap_mux);
    strlcpy(s_snap_ssid, ssid, sizeof(s_snap_ssid));
    s_source = source;
    taskEXIT_CRITICAL(&s_snap_mux);
}

void wifi_job_get(wifi_job_t *out)
{
    taskENTER_CRITICAL(&s_snap_mux);
    *out = s_job;
    taskEXIT_CRITICAL(&s_snap_mux);
}

void wifi_current_ssid(char out[WIFI_SSID_LEN])
{
    taskENTER_CRITICAL(&s_snap_mux);
    memcpy(out, s_snap_ssid, WIFI_SSID_LEN);
    taskEXIT_CRITICAL(&s_snap_mux);
}

const char *wifi_source(void)
{
    return s_source; // a pointer to a literal, written whole
}

bool wifi_file_present(void)
{
    return s_file_present;
}

size_t wifi_scan_results(wifiscan_entry_t *out, size_t max)
{
    taskENTER_CRITICAL(&s_snap_mux);
    const size_t n = s_scan_n < max ? s_scan_n : max;
    memcpy(out, s_scan, n * sizeof(out[0]));
    taskEXIT_CRITICAL(&s_snap_mux);
    return n;
}

/* ---- The connect machine's glue ---- */

static void set_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    // Lengths are validated by the callers; a full-length field needs no NUL.
    memcpy(cfg.sta.ssid, ssid, strlen(ssid));
    memcpy(cfg.sta.password, pass, strlen(pass));
    if (pass[0] != '\0') {
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK; // don't join an open AP with the same SSID
    }
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
    }
    memset(&cfg, 0, sizeof(cfg));
}

static void trial_forget(void)
{
    memset(s_try_ssid, 0, sizeof(s_try_ssid));
    memset(s_try_pass, 0, sizeof(s_try_pass));
    memset(&s_req, 0, sizeof(s_req));
    logcap_set_candidate_password("");
}

static void save_current_job(void *arg);
static void rssi_timer_cb(void *arg);

// Runs one event through the machine and does what it asks. Called with s_lock held; `note` gets the status text to
// show once the lock is released.
static void step_locked(wifi_conn_ev_t ev, int arg, char *note, size_t note_size)
{
    const wifi_conn_state_t before = s_m.state;
    const wifi_step_t out = wifistate_step(&s_m, ev, arg);
    const uint32_t a = out.actions;
    if (s_m.state != before) {
        ESP_LOGI(TAG, "state %s -> %s", wifistate_name(before), wifistate_name(s_m.state));
    }

    char cur[WIFI_SSID_LEN], tried[WIFI_SSID_LEN];
    wifiscan_printable(cur, sizeof(cur), s_cur_ssid);
    wifiscan_printable(tried, sizeof(tried), s_try_ssid);

    if (a & WA_STOP_RETRY) {
        esp_timer_stop(s_retry_timer); // INVALID_STATE when it isn't armed
    }
    if (a & WA_STOP_TRIAL_TIMER) {
        esp_timer_stop(s_trial_timer);
    }
    if (a & WA_DISCONNECT) {
        esp_wifi_disconnect(); // an error only says there was nothing to leave
    }
    if (a & WA_SET_CURRENT_CONFIG) {
        set_config(s_cur_ssid, s_cur_pass);
    }
    if (a & WA_SET_TRIAL_CONFIG) {
        set_config(s_try_ssid, s_try_pass);
    }
    if (a & WA_CONNECT) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        }
    }
    if (a & WA_ARM_RETRY) {
        const esp_err_t err = esp_timer_start_once(s_retry_timer, RECONNECT_DELAY_US);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { // INVALID_STATE: retry already armed
            ESP_LOGE(TAG, "retry timer: %s", esp_err_to_name(err));
        }
    }
    if (a & WA_START_TRIAL_TIMER) {
        const esp_err_t err = esp_timer_start_once(s_trial_timer, TRIAL_TIMEOUT_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "trial timer: %s", esp_err_to_name(err));
        }
        snprintf(note, note_size, "trying %s", tried);
    }

    if (ev == WE_DISCONNECTED && s_m.state == WS_BACKOFF) {
        snprintf(note, note_size, "connecting to %s (retry, reason %d)", cur, arg);
    } else if (ev == WE_DISCONNECTED && s_m.state == WS_TRIAL) {
        ESP_LOGI(TAG, "trial: disconnected, reason %d", arg);
    }

    if ((a & WA_REFUSED) && ev == WE_TRIAL_BEGIN) {
        ESP_LOGW(TAG, "trial refused: a trial or restore is running");
        trial_forget();
        job_done(false, out.fail, WIFI_SAVE_NONE, ESP_OK);
    }
    if (a & WA_TRIAL_FAILED) {
        ESP_LOGW(TAG, "trial of %s failed: %s", tried, wifistate_fail_text(out.fail));
        snprintf(note, note_size, "could not join %s (%s)", tried, wifistate_fail_text(out.fail));
        trial_forget();
        job_done(false, out.fail, WIFI_SAVE_NONE, ESP_OK);
    }
    if (a & WA_TRIAL_OK) {
        // The trial's network is the current one now; its password goes from the candidate slot to the saved one.
        strlcpy(s_cur_ssid, s_try_ssid, sizeof(s_cur_ssid));
        strlcpy(s_cur_pass, s_try_pass, sizeof(s_cur_pass));
        logcap_set_password(s_cur_pass);
        trial_forget();
        job_joined(s_cur_ssid);
        ESP_LOGI(TAG, "trial of %s joined, saving wifi.txt", tried);
        if (svc_post(save_current_job, NULL) != ESP_OK) {
            job_done(true, WF_NONE, WIFI_SAVE_BUSY, ESP_OK);
        }
    }
    s_state_mirror = s_m.state;
}

static void dispatch(wifi_conn_ev_t ev, int arg)
{
    char note[NOTE_MAX] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    step_locked(ev, arg, note, sizeof(note));
    xSemaphoreGive(s_lock);
    if (note[0]) {
        report("%s", note);
    }
}

static void retry_cb(void *arg)
{
    dispatch(WE_RETRY_TIMER, s_scanning);
}

static void trial_timeout_cb(void *arg)
{
    dispatch(WE_TRIAL_TIMEOUT, 0);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }
    const wifi_event_sta_disconnected_t *ev = data;
    // 8 is our own esp_wifi_disconnect ahead of a trial or a restore, unless the board was connected and no one asked.
    if (ev->reason != 8 || s_state_mirror == WS_CONNECTED) {
        diag_count(DIAG_WIFI_DROP, wifi_reason_name(ev->reason));
    }
    dispatch(WE_DISCONNECTED, ev->reason);
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));

    char note[NOTE_MAX] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A trial counts only for its own SSID, a restore for the old one; any other IP (a straggler of the network we
    // just left) leaves the machine alone.
    int matches = 1;
    if (s_m.state == WS_TRIAL || s_m.state == WS_RESTORING) {
        wifi_ap_record_t ap;
        const char *want = s_m.state == WS_TRIAL ? s_try_ssid : s_cur_ssid;
        matches = esp_wifi_sta_get_ap_info(&ap) == ESP_OK && strcmp((const char *)ap.ssid, want) == 0;
    }
    step_locked(WE_GOT_IP, matches, note, sizeof(note));
    if (s_m.state == WS_CONNECTED) {
        snprintf(note, sizeof(note), "connected " IPSTR " (" HOSTNAME ".local)", IP2STR(&ev->ip_info.ip));
    }
    xSemaphoreGive(s_lock);
    if (note[0]) {
        report("%s", note);
    }
}

esp_err_t wifi_init(wifi_status_cb_t status_cb)
{
    s_status_cb = status_cb;
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    wifistate_init(&s_m);

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }
    start_mdns(sta);
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "esp_wifi_init");

    const esp_timer_create_args_t retry_args = {.callback = retry_cb, .name = "wifi_retry"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&retry_args, &s_retry_timer), TAG, "timer");
    const esp_timer_create_args_t trial_args = {.callback = trial_timeout_cb, .name = "wifi_trial"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&trial_args, &s_trial_timer), TAG, "timer");
    const esp_timer_create_args_t rssi_args = {.callback = rssi_timer_cb, .name = "wifi_rssi"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&rssi_args, &s_rssi_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL), TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL), TAG, "ip evt");

    // Station mode with no config: nothing connects until wifi_apply_saved or a trial sets one, but a scan works.
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");
    return ESP_OK;
}

// Join the network. Called once at boot with the saved credentials.
static esp_err_t wifi_apply(const char *ssid, const char *pass, const char *source)
{
    char shown[WIFI_SSID_LEN];
    wifiscan_printable(shown, sizeof(shown), ssid);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur_ssid, ssid, sizeof(s_cur_ssid));
    strlcpy(s_cur_pass, pass, sizeof(s_cur_pass));
    xSemaphoreGive(s_lock);
    snapshot_network(ssid, source);
    logcap_set_password(pass); // so the log ring redacts it

    report("connecting to %s (source: %s)", shown, source);
    dispatch(WE_APPLY, 0);
    return ESP_OK;
}

esp_err_t wifi_apply_saved(void)
{
    char ssid[SSID_MAX + 1] = "";
    char pass[PASS_MAX + 1] = "";
    const char *source = "sd";
    if (read_card_credentials(ssid, pass)) {
        s_file_present = true;
    } else {
        source = "config";
        if (strlen(CONFIG_WATCHDECK_WIFI_SSID) > SSID_MAX || strlen(CONFIG_WATCHDECK_WIFI_PASSWORD) > PASS_MAX) {
            ESP_LOGE(TAG, "Kconfig SSID or password is too long");
            sdcard_release_fs();
            report("invalid Wi-Fi credentials");
            return ESP_ERR_INVALID_ARG;
        }
        strcpy(ssid, CONFIG_WATCHDECK_WIFI_SSID);
        strcpy(pass, CONFIG_WATCHDECK_WIFI_PASSWORD);
    }
    // The card goes to the USB host once wifi.txt has been read.
    sdcard_release_fs();

    // An empty SSID would join any open AP, so refuse.
    if (ssid[0] == '\0') {
        report("no Wi-Fi credentials: set them in Settings > Wi-Fi");
        return ESP_ERR_INVALID_ARG;
    }
    return wifi_apply(ssid, pass, source);
}

/* ---- Jobs on svc ---- */

static wifi_save_t save_result(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return WIFI_SAVE_OK;
    case CARDFS_ERR_DIR_FULL:
        return WIFI_SAVE_DIR_FULL;
    case ESP_ERR_INVALID_STATE: // not writable, or tainted: the mount's owner says which
        if (!sdcard_card()) {
            return WIFI_SAVE_NO_CARD;
        }
        return cardfs_writable() ? WIFI_SAVE_TAINTED : WIFI_SAVE_SHARED;
    default:
        return WIFI_SAVE_ERROR;
    }
}

// Write wifi.txt. The password reaches no log.
static wifi_save_t write_wifi_txt(const char *ssid, const char *pass)
{
    char buf[WIFITXT_FILE_MAX];
    const size_t len = wifitxt_format(ssid, pass, buf);
    if (len == 0) {
        return WIFI_SAVE_ERROR; // wifi_try and wifi_save refused this already
    }
    const esp_err_t err = cardfs_write_small("wifi.txt", buf, len);
    memset(buf, 0, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi.txt not written: %s (0x%x)", esp_err_to_name(err), (unsigned)err);
    }
    return save_result(err);
}

// After a trial joined: write the credentials it made current.
static void save_current_job(void *arg)
{
    char ssid[WIFI_SSID_LEN], pass[WIFITXT_PASS_MAX + 1];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(ssid, s_cur_ssid, sizeof(ssid));
    memcpy(pass, s_cur_pass, sizeof(pass));
    xSemaphoreGive(s_lock);
    const wifi_save_t save = write_wifi_txt(ssid, pass);
    memset(pass, 0, sizeof(pass));
    if (save == WIFI_SAVE_OK) {
        snapshot_network(ssid, "sd");
        s_file_present = true;
    }
    job_done(true, WF_NONE, save, ESP_OK);
}

static void save_only_job(void *arg)
{
    char ssid[WIFI_SSID_LEN], pass[WIFITXT_PASS_MAX + 1];
    memcpy(ssid, s_req.ssid, sizeof(ssid)); // the claim keeps every other writer out until job_done
    memcpy(pass, s_req.pass, sizeof(pass));
    memset(&s_req, 0, sizeof(s_req));
    const wifi_save_t save = write_wifi_txt(ssid, pass);
    memset(pass, 0, sizeof(pass));
    if (save == WIFI_SAVE_OK) {
        s_file_present = true;
    }
    job_done(save == WIFI_SAVE_OK, WF_NONE, save, ESP_OK);
}

static void trial_begin_job(void *arg)
{
    char note[NOTE_MAX] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_try_ssid, s_req.ssid, sizeof(s_try_ssid));
    strlcpy(s_try_pass, s_req.pass, sizeof(s_try_pass));
    memset(&s_req, 0, sizeof(s_req));
    step_locked(WE_TRIAL_BEGIN, s_cur_ssid[0] != '\0', note, sizeof(note));
    xSemaphoreGive(s_lock);
    if (note[0]) {
        report("%s", note);
    }
}

static void scan_job(void *arg)
{
    wifiscan_entry_t list[WIFISCAN_MAX];
    size_t count = 0;
    esp_err_t err;

    s_scanning = true;
    err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        uint16_t n = 0;
        err = esp_wifi_scan_get_ap_num(&n);
        if (err == ESP_OK && n > SCAN_RECORDS) {
            n = SCAN_RECORDS;
        }
        wifi_ap_record_t *recs =
            err == ESP_OK && n > 0 ? heap_caps_calloc(n, sizeof(*recs), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : NULL;
        if (recs) {
            err = esp_wifi_scan_get_ap_records(&n, recs); // also frees the driver's list
            for (uint16_t i = 0; err == ESP_OK && i < n; i++) {
                count = wifiscan_add(list, count, WIFISCAN_MAX, (const char *)recs[i].ssid, recs[i].rssi,
                                     recs[i].authmode != WIFI_AUTH_OPEN);
            }
            heap_caps_free(recs);
        } else if (n > 0) {
            esp_wifi_clear_ap_list();
            err = ESP_ERR_NO_MEM;
        }
    }
    s_scanning = false;

    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_snap_mux);
        memcpy(s_scan, list, count * sizeof(list[0]));
        s_scan_n = count;
        taskEXIT_CRITICAL(&s_snap_mux);
        ESP_LOGI(TAG, "scan found %u network(s)", (unsigned)count);
    } else {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
    }
    job_done(err == ESP_OK, WF_NONE, WIFI_SAVE_NONE, err);
}

esp_err_t wifi_scan(void)
{
    if (!s_lock || s_state_mirror == WS_TRIAL || s_state_mirror == WS_RESTORING || !job_claim(WIFI_JOB_SCAN)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = svc_post(scan_job, NULL);
    if (err != ESP_OK) {
        job_done(false, WF_NONE, WIFI_SAVE_NONE, err);
    }
    return err;
}

// Start a job that carries credentials: validate them, claim the slot, hand them to the svc job through s_req.
static esp_err_t start_credential_job(wifi_job_kind_t kind, const char *ssid, const char *pass, svc_job_t fn)
{
    char buf[WIFITXT_FILE_MAX];
    const bool valid = wifitxt_format(ssid, pass, buf) != 0;
    memset(buf, 0, sizeof(buf));
    if (!valid) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock || !job_claim(kind)) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_req.ssid, ssid, sizeof(s_req.ssid));
    strlcpy(s_req.pass, pass, sizeof(s_req.pass));
    if (kind == WIFI_JOB_TRIAL) {
        logcap_set_candidate_password(pass); // before the first driver call, so no log line can carry it
    }
    const esp_err_t err = svc_post(fn, NULL);
    if (err != ESP_OK) {
        memset(&s_req, 0, sizeof(s_req));
        logcap_set_candidate_password("");
        job_done(false, WF_NONE, WIFI_SAVE_NONE, err);
    }
    return err;
}

esp_err_t wifi_try(const char *ssid, const char *pass)
{
    return start_credential_job(WIFI_JOB_TRIAL, ssid, pass, trial_begin_job);
}

esp_err_t wifi_save(const char *ssid, const char *pass)
{
    return start_credential_job(WIFI_JOB_SAVE, ssid, pass, save_only_job);
}

/* ---- RSSI ---- */

static void rssi_job(void *arg)
{
    s_rssi_queued = false;
    if (s_scanning || s_state_mirror != WS_CONNECTED) {
        return;
    }
    const int64_t start_us = esp_timer_get_time();
    int dbm;
    const esp_err_t err = esp_wifi_sta_get_rssi(&dbm);
    const int64_t now = esp_timer_get_time();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RSSI read: %s", esp_err_to_name(err));
        return;
    }
    taskENTER_CRITICAL(&s_snap_mux);
    s_rssi_valid = true;
    s_rssi_dbm = dbm;
    s_rssi_at_us = now;
    taskEXIT_CRITICAL(&s_snap_mux);
    if (!s_rssi_timed) {
        s_rssi_timed = true;
        ESP_LOGI(TAG, "RSSI read took %d us", (int)(now - start_us));
    }
}

static void post_rssi(void)
{
    if (s_scanning || s_rssi_queued || s_state_mirror != WS_CONNECTED) {
        return;
    }
    s_rssi_queued = true;
    if (svc_post(rssi_job, NULL) != ESP_OK) {
        s_rssi_queued = false;
    }
}

static void rssi_timer_cb(void *arg)
{
    post_rssi();
}

void wifi_watch_rssi(bool on)
{
    if (!s_rssi_timer) {
        return;
    }
    if (on) {
        esp_timer_start_periodic(s_rssi_timer, RSSI_REFRESH_US); // INVALID_STATE: already running
        post_rssi();
    } else {
        esp_timer_stop(s_rssi_timer);
    }
}

bool wifi_rssi(int *dbm, uint32_t *age_ms)
{
    taskENTER_CRITICAL(&s_snap_mux);
    const bool valid = s_rssi_valid;
    *dbm = s_rssi_dbm;
    const int64_t at_us = s_rssi_at_us;
    taskEXIT_CRITICAL(&s_snap_mux);
    *age_ms = (uint32_t)((esp_timer_get_time() - at_us) / 1000);
    return valid;
}

bool wifi_ip(char *out, size_t out_size)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (!sta || esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    snprintf(out, out_size, IPSTR, IP2STR(&info.ip));
    return true;
}
