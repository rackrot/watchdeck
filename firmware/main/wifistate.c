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
// The driver's other "no AP matched" reasons: no compatible security, outside the auth-mode threshold, outside the
// RSSI threshold.
#define REASON_NO_AP_COMPATIBLE_SECURITY 210
#define REASON_NO_AP_AUTHMODE_THRESHOLD 211
#define REASON_NO_AP_RSSI_THRESHOLD 212

static bool means_not_found(int reason)
{
    return reason == REASON_NO_AP_FOUND || reason == REASON_NO_AP_COMPATIBLE_SECURITY ||
           reason == REASON_NO_AP_AUTHMODE_THRESHOLD || reason == REASON_NO_AP_RSSI_THRESHOLD;
}

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

// The trial ends. With a network to go back to, reconnect to it under the same 20 s clock; without one, stop trying
// anything.
static wifi_step_t end_trial(wifi_machine_t *m, wifi_fail_t fail)
{
    wifi_step_t out = {.actions = WA_STOP_TRIAL_TIMER | WA_DISCONNECT | WA_TRIAL_FAILED, .fail = fail};
    m->joined = false;
    if (m->has_old) {
        m->state = WS_RESTORING;
        m->restore_grace = fail == WF_TIMEOUT; // the connect we just cut short may still report its failure
        out.actions |= WA_SET_CURRENT_CONFIG | WA_CONNECT | WA_START_TRIAL_TIMER;
    } else {
        m->state = WS_IDLE;
    }
    return out;
}

static wifi_step_t on_disconnect(wifi_machine_t *m, int reason)
{
    wifi_step_t out = {0};
    if (m->state == WS_TRIAL || m->state == WS_RESTORING) {
        m->joined = false; // whatever IP comes next is not from the link that just went down
    }
    if (reason == REASON_ASSOC_LEAVE && m->state != WS_CONNECTED) {
        return out; // our own disconnect, ahead of a trial, a restore or an apply
    }
    switch (m->state) {
    case WS_IDLE:
        break;
    case WS_CONNECTING:
    case WS_CONNECTED:
    case WS_BACKOFF:
        m->state = WS_BACKOFF;
        out.actions = WA_ARM_RETRY;
        break;
    case WS_RESTORING:
        if (m->restore_grace) {
            m->restore_grace = false; // the trial's own last attempt reporting in; the restore connect is in flight
            break;
        }
        // The old network didn't come back either: the normal retry takes over with its credentials.
        m->state = WS_BACKOFF;
        out.actions = WA_STOP_TRIAL_TIMER | WA_ARM_RETRY;
        break;
    case WS_TRIAL:
        // The retry timer never arms here. Connect again until the same kind of failure repeats, or the timer ends it.
        if (points_at_password(reason)) {
            m->wrong_pass++;
        } else if (means_not_found(reason)) {
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

    case WE_CONNECTED:
        if ((m->state == WS_TRIAL || m->state == WS_RESTORING) && arg) {
            m->joined = true;
        }
        break;

    case WE_GOT_IP:
        if (m->state == WS_TRIAL) {
            if (m->joined) { // an IP before the trial's own CONNECTED is the old network's, late
                m->joined = false;
                m->state = WS_CONNECTED;
                m->has_old = true; // the trial's network is the current one now
                out.actions = WA_STOP_TRIAL_TIMER | WA_TRIAL_OK;
            }
        } else if (m->state == WS_RESTORING) {
            if (m->joined) {
                m->joined = false;
                m->state = WS_CONNECTED;
                out.actions = WA_STOP_TRIAL_TIMER; // the restore deadline
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
                out.actions = WA_SET_CURRENT_CONFIG | WA_CONNECT; // the driver may still hold a failed trial's config
            }
        } // any other state: the timer fired after it was stopped
        break;

    case WE_TRIAL_BEGIN:
        if (m->state == WS_TRIAL) {
            out.actions = WA_REFUSED;
            out.fail = WF_BUSY;
            break;
        }
        // From RESTORING this supersedes the restore: the old network is still the one to go back to.
        out.actions = WA_STOP_RETRY | WA_STOP_TRIAL_TIMER | WA_SET_TRIAL_CONFIG | WA_CONNECT | WA_START_TRIAL_TIMER;
        if (m->state == WS_CONNECTED || m->state == WS_CONNECTING || m->state == WS_RESTORING) {
            out.actions |= WA_DISCONNECT;
        }
        m->state = WS_TRIAL;
        m->has_old = arg != 0;
        m->wrong_pass = 0;
        m->not_found = 0;
        m->joined = false;
        m->restore_grace = false;
        break;

    case WE_TRIAL_TIMEOUT:
        if (m->state == WS_TRIAL) {
            // A repeated sign of one kind before the clock ran out still says more than "timed out".
            out = end_trial(m, m->wrong_pass ? WF_WRONG_PASSWORD : m->not_found ? WF_NOT_FOUND : WF_TIMEOUT);
        } else if (m->state == WS_RESTORING) {
            // The old network didn't answer in time: the normal retry takes over.
            m->state = WS_BACKOFF;
            m->joined = false;
            m->restore_grace = false;
            out.actions = WA_DISCONNECT | WA_ARM_RETRY;
        }
        break;

    case WE_DRIVER_ERROR:
        // No disconnect event follows a call the driver refused, so the machine moves on by itself.
        if (m->state == WS_TRIAL) {
            out = end_trial(m, WF_ERROR);
        } else if (m->state != WS_IDLE) {
            m->state = WS_BACKOFF;
            m->joined = false;
            m->restore_grace = false;
            out.actions = WA_STOP_TRIAL_TIMER | WA_ARM_RETRY;
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
    case WF_ERROR:
        return "error";
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
