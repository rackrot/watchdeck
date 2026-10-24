#pragma once

/*
 * The Wi-Fi connect state machine, as a pure function of the state and an event, and the scan list's merge. wifi.c runs
 * it under one mutex, from the Wi-Fi event handler, the retry and trial timers and the svc task, and does what the
 * returned actions say. Nothing else calls esp_wifi_connect or esp_wifi_disconnect, so a trial connect and the retry
 * timer can never both connect. No ESP-IDF includes, so the host tests compile it.
 *
 *   IDLE        no network to join: no credentials yet, or a failed trial with nothing to go back to
 *   CONNECTING  a connect is in flight (boot, or the retry timer fired)
 *   CONNECTED   has an IP
 *   BACKOFF     disconnected, the retry timer is armed
 *   TRIAL       a connect with the typed credentials; the retry timer is stopped and never armed
 *   RESTORING   the trial failed, reconnecting to the old network; a failure here falls to BACKOFF
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    WS_IDLE,
    WS_CONNECTING,
    WS_CONNECTED,
    WS_BACKOFF,
    WS_TRIAL,
    WS_RESTORING,
} wifi_conn_state_t;

typedef enum {
    WE_APPLY,         // connect with the current credentials (boot)
    WE_DISCONNECTED,  // arg: the reason number
    WE_GOT_IP,        // arg: 1 when the SSID joined is the one this state expects (the trial's, or the old one)
    WE_RETRY_TIMER,   // arg: 1 while a scan runs, which a connect would abort
    WE_TRIAL_BEGIN,   // arg: 1 when there is an old network to go back to
    WE_TRIAL_TIMEOUT, // the 20 s timer
} wifi_conn_ev_t;

// What the glue does, in this order: stop the retry timer, stop the trial timer, disconnect, set the config (the old
// one or the trial's), connect, arm the retry timer, start the trial timer, then report the trial's end.
#define WA_STOP_RETRY (1u << 0)
#define WA_STOP_TRIAL_TIMER (1u << 1)
#define WA_DISCONNECT (1u << 2)
#define WA_SET_CURRENT_CONFIG (1u << 3)
#define WA_SET_TRIAL_CONFIG (1u << 4)
#define WA_CONNECT (1u << 5)
#define WA_ARM_RETRY (1u << 6)
#define WA_START_TRIAL_TIMER (1u << 7)
#define WA_TRIAL_OK (1u << 8)     // joined the trial's network: make its credentials the current ones and save them
#define WA_TRIAL_FAILED (1u << 9) // out.fail says why
#define WA_REFUSED (1u << 10)     // WE_TRIAL_BEGIN or WE_APPLY while a trial or a restore runs; out.fail is WF_BUSY

typedef enum {
    WF_NONE,
    WF_WRONG_PASSWORD,
    WF_NOT_FOUND,
    WF_TIMEOUT,
    WF_BUSY,
} wifi_fail_t;

typedef struct {
    wifi_conn_state_t state;
    bool has_old;       // a trial can go back to a network
    uint8_t wrong_pass; // this trial's disconnects that point at the password
    uint8_t not_found;  // and at a missing network
} wifi_machine_t;

typedef struct {
    uint32_t actions;
    wifi_fail_t fail;
} wifi_step_t;

void wifistate_init(wifi_machine_t *m);
wifi_step_t wifistate_step(wifi_machine_t *m, wifi_conn_ev_t ev, int arg);

const char *wifistate_name(wifi_conn_state_t s);
// "wrong password?", "network not found", "timed out", "busy". "" for WF_NONE.
const char *wifistate_fail_text(wifi_fail_t f);

/* ---- The scan list ---- */

#define WIFISCAN_MAX 15
#define WIFISCAN_SSID_MAX 32

typedef struct {
    char ssid[WIFISCAN_SSID_MAX + 1]; // the raw bytes, which may not be printable
    int8_t rssi;
    bool secure;
} wifiscan_entry_t;

// Adds one scan record to a list of n entries sorted by signal, strongest first, and returns the new n. A hidden
// network (empty SSID) is skipped; one seen twice keeps its stronger reading; a full list drops its weakest.
size_t wifiscan_add(wifiscan_entry_t *list, size_t n, size_t cap, const char *ssid, int rssi, bool secure);

// Copies `ssid` with every byte outside 0x20..0x7E turned into '?', cut to dst_size - 1.
void wifiscan_printable(char *dst, size_t dst_size, const char *ssid);
