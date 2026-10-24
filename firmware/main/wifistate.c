#include "wifistate.h"

#include <string.h>

// WIFI_REASON_* numbers. The ones that point at the password are the 4-way handshake timeout (what a wrong WPA key
// usually gives), a MIC failure, an authentication failure and the handshake timeout. ASSOC_LEAVE is what the driver
// reports for our own esp_wifi_disconnect.
#define REASON_MIC_FAILURE 14
#define REASON_4WAY_TIMEOUT 15
#define REASON_ASSOC_LEAVE 8
#define REASON_NO_AP_FOUND 201
#define REASON_AUTH_FAIL 202
#define REASON_HANDSHAKE_TIMEOUT 204

static bool points_at_password(int reason)
{
    return reason == REASON_4WAY_TIMEOUT || reason == REASON_MIC_FAILURE || reason == REASON_AUTH_FAIL ||
           reason == REASON_HANDSHAKE_TIMEOUT;
}

void wifistate_init(wifi_machine_t *m)
{
    memset(m, 0, sizeof(*m));
    m->state = WS_IDLE;
}

// The trial ends. With a network to go back to, reconnect to it; without one, stop trying anything.
static wifi_step_t end_trial(wifi_machine_t *m, wifi_fail_t fail)
{
    wifi_step_t out = {.actions = WA_STOP_TRIAL_TIMER | WA_DISCONNECT | WA_TRIAL_FAILED, .fail = fail};
    if (m->has_old) {
        m->state = WS_RESTORING;
        out.actions |= WA_SET_CURRENT_CONFIG | WA_CONNECT;
    } else {
        m->state = WS_IDLE;
    }
    return out;
}

static wifi_step_t on_disconnect(wifi_machine_t *m, int reason)
{
    wifi_step_t out = {0};
    if (reason == REASON_ASSOC_LEAVE && m->state != WS_CONNECTED) {
        return out; // our own disconnect, ahead of a trial, a restore or an apply
    }
    switch (m->state) {
    case WS_IDLE:
        break;
    case WS_CONNECTING:
    case WS_CONNECTED:
    case WS_BACKOFF:
    case WS_RESTORING: // the old network didn't come back either: the normal retry takes over with its credentials
        m->state = WS_BACKOFF;
        out.actions = WA_ARM_RETRY;
        break;
    case WS_TRIAL:
        // The retry timer never arms here. Connect again until the same kind of failure repeats, or the timer ends it.
        if (points_at_password(reason)) {
            m->wrong_pass++;
        } else if (reason == REASON_NO_AP_FOUND) {
            m->not_found++;
        }
        if (m->wrong_pass >= 2) {
            return end_trial(m, WF_WRONG_PASSWORD);
        }
        if (m->not_found >= 2) {
            return end_trial(m, WF_NOT_FOUND);
        }
        out.actions = WA_CONNECT;
        break;
    }
    return out;
}

wifi_step_t wifistate_step(wifi_machine_t *m, wifi_conn_ev_t ev, int arg)
{
    wifi_step_t out = {0};
    switch (ev) {
    case WE_APPLY:
        if (m->state == WS_TRIAL || m->state == WS_RESTORING) {
            out.actions = WA_REFUSED;
            out.fail = WF_BUSY;
            break;
        }
        out.actions = WA_SET_CURRENT_CONFIG | WA_CONNECT;
        if (m->state == WS_BACKOFF) {
            out.actions |= WA_STOP_RETRY;
        } else if (m->state != WS_IDLE) {
            out.actions |= WA_STOP_RETRY | WA_DISCONNECT;
        }
        m->state = WS_CONNECTING;
        break;

    case WE_DISCONNECTED:
        out = on_disconnect(m, arg);
        break;

    case WE_GOT_IP:
        if (m->state == WS_TRIAL) {
            if (arg) {
                m->state = WS_CONNECTED;
                m->has_old = true; // the trial's network is the current one now
                out.actions = WA_STOP_TRIAL_TIMER | WA_TRIAL_OK;
            }
        } else if (m->state == WS_RESTORING) {
            if (arg) {
                m->state = WS_CONNECTED;
            }
        } else {
            m->state = WS_CONNECTED;
            out.actions = WA_STOP_RETRY;
        }
        break;

    case WE_RETRY_TIMER:
        if (m->state == WS_BACKOFF) {
            if (arg) {
                out.actions = WA_ARM_RETRY; // a scan runs: look again in a moment
            } else {
                m->state = WS_CONNECTING;
                out.actions = WA_CONNECT;
            }
        } // any other state: the timer fired after it was stopped
        break;

    case WE_TRIAL_BEGIN:
        if (m->state == WS_TRIAL || m->state == WS_RESTORING) {
            out.actions = WA_REFUSED;
            out.fail = WF_BUSY;
            break;
        }
        out.actions = WA_STOP_RETRY | WA_SET_TRIAL_CONFIG | WA_CONNECT | WA_START_TRIAL_TIMER;
        if (m->state == WS_CONNECTED || m->state == WS_CONNECTING) {
            out.actions |= WA_DISCONNECT;
        }
        m->state = WS_TRIAL;
        m->has_old = arg != 0;
        m->wrong_pass = 0;
        m->not_found = 0;
        break;

    case WE_TRIAL_TIMEOUT:
        if (m->state == WS_TRIAL) {
            // A repeated sign of one kind before the clock ran out still says more than "timed out".
            out = end_trial(m, m->wrong_pass ? WF_WRONG_PASSWORD : m->not_found ? WF_NOT_FOUND : WF_TIMEOUT);
        }
        break;
    }
    return out;
}

const char *wifistate_name(wifi_conn_state_t s)
{
    switch (s) {
    case WS_IDLE:
        return "idle";
    case WS_CONNECTING:
        return "connecting";
    case WS_CONNECTED:
        return "connected";
    case WS_BACKOFF:
        return "backoff";
    case WS_TRIAL:
        return "trial";
    case WS_RESTORING:
        return "restoring";
    }
    return "?";
}

const char *wifistate_fail_text(wifi_fail_t f)
{
    switch (f) {
    case WF_WRONG_PASSWORD:
        return "wrong password?";
    case WF_NOT_FOUND:
        return "network not found";
    case WF_TIMEOUT:
        return "timed out";
    case WF_BUSY:
        return "busy";
    case WF_NONE:
        break;
    }
    return "";
}

size_t wifiscan_add(wifiscan_entry_t *list, size_t n, size_t cap, const char *ssid, int rssi, bool secure)
{
    if (ssid[0] == '\0' || strlen(ssid) > WIFISCAN_SSID_MAX || cap == 0) {
        return n;
    }
    if (rssi < -128) {
        rssi = -128;
    } else if (rssi > 0) {
        rssi = 0;
    }
    // A network seen twice (two access points, or two channels) keeps its stronger reading.
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i].ssid, ssid) == 0) {
            if (rssi <= list[i].rssi) {
                return n;
            }
            memmove(&list[i], &list[i + 1], (n - i - 1) * sizeof(list[0]));
            n--;
            break;
        }
    }
    size_t at = 0;
    while (at < n && list[at].rssi >= rssi) {
        at++;
    }
    if (at >= cap) {
        return n; // weaker than every entry of a full list
    }
    if (n == cap) {
        n--; // drop the weakest
    }
    memmove(&list[at + 1], &list[at], (n - at) * sizeof(list[0]));
    strcpy(list[at].ssid, ssid);
    list[at].rssi = (int8_t)rssi;
    list[at].secure = secure;
    return n + 1;
}

void wifiscan_printable(char *dst, size_t dst_size, const char *ssid)
{
    size_t i = 0;
    for (; ssid[i] != '\0' && i + 1 < dst_size; i++) {
        const unsigned char c = (unsigned char)ssid[i];
        dst[i] = c >= 0x20 && c <= 0x7E ? (char)c : '?';
    }
    if (dst_size > 0) {
        dst[i] = '\0';
    }
}
