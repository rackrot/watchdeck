#include "link.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "crash.h"
#include "diag.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "mdns.h"
#include "icons.h"
#include "logcap.h"
#include "model.h"
#include "pageset.h"
#include "pairing.h"

#define SERVICE "_watchdeck"
#define SERVICE_PROTO "_tcp"
#define QUEUE_LEN 16
#define TASK_STACK 8192
#define WS_TASK_STACK 6144 // also runs the TLS handshake
#define DISCOVER_TIMEOUT_MS 3000
#define DISCOVER_BUDGET_MS 10000 // all per-instance TXT and A queries of one pass
#define MAX_CANDIDATES 10        // also the PTR result cap
#define CONNECT_TIMEOUT_MS 15000
#define SEND_TIMEOUT_MS 2000
#define PING_INTERVAL_S 10
#define PINGPONG_TIMEOUT_S 25
#define BROWSE_INTERVAL_MS 4000  // unpaired: pause between looks for daemons open for pairing
#define PAIR_TIMEOUT_MS 130000   // one attempt, tap to verdict; the daemon's own window is 120 s
#define CONFIRM_TIMEOUT_MS 60000 // then the person's Yes or No on the device
#define RX_POST_WAIT_MS                                                                                                \
    500 // a full queue holds the websocket task this long before a frame is dropped; under SEND_TIMEOUT_MS
#define WS_OP_TEXT 0x1
#define WS_OP_BINARY 0x2
// An icon_get with no frame of it for this long, since it went out or since its last frame, is given up: the next
// goes out, and what didn't come is left out. A 238 px icon takes about 0.5 s, so 16 of them outlast a fixed 5 s.
#define ICON_GET_TIMEOUT_MS 5000
#define ICON_GET_BUF_LEN 2048 // 40 ids of 32 digits fit with room to spare

static const char *TAG = "link";

// Backoff steps in seconds; the last one is the cap. Each wait gets +/-20% jitter.
static const uint32_t BACKOFF_S[] = {1, 2, 4, 8, 16, 30};

typedef enum {
    ST_NO_IP,      // waiting for Wi-Fi
    ST_BACKOFF,    // paired: waiting to retry (retry_at_ms)
    ST_CONNECTING, // paired: client started, no CONNECTED yet
    ST_UP,         // paired: hello sent
    ST_BROWSE,     // unpaired: listing daemons open for pairing (browse_at_ms)
    ST_PAIRING,    // unpaired: an attempt on a daemon's pairing port
    ST_CONFIRM,    // unpaired: the daemon said ok, and the person must confirm it on the device
} state_t;

typedef enum {
    EV_GOT_IP,
    EV_WIFI_DOWN,
    EV_WS_UP,
    EV_WS_DOWN,
    EV_WS_MSG,      // data is a PSRAM buffer the link task frees
    EV_WS_BAD,      // oversize or broken frame: close the link
    EV_WS_BAD_CERT, // the daemon's cert is not the pinned one
    EV_PRESS,
    EV_FEED_DISMISS, // a note id in press.btn
    EV_PAIR,         // the user picked a daemon to pair with
    EV_PAIR_CANCEL,  // the user abandoned the attempt, or said No to its question
    EV_PAIR_CONFIRM, // the user said Yes, the desktop shows the pairing
    EV_FORGET,       // the user asked to forget the pairing
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint32_t gen; // websocket events carry the client generation they came from
    union {
        struct {
            char *data;
            size_t len;
            bool binary; // an icon frame, else a JSON text message
        } msg;
        struct {
            char btn[PROTO_ID_MAX];
            uint32_t seq;
        } press;
        link_daemon_t daemon;
    };
} link_ev_t;

typedef struct {
    esp_ip4_addr_t ip;
    uint16_t port;
    bool local; // on our own subnet
} cand_t;

// Everything below except s_rx is touched only by the link task.
static struct {
    QueueHandle_t queue;
    link_cbs_t cbs;
    proto_msg_t *msg;           // parse target, in PSRAM
    pairing_t *pairing;         // in PSRAM; while paired, the client points at its cert_pem
    pairing_session_t *session; // in PSRAM, the attempt in ST_PAIRING and ST_CONFIRM
    bool paired;
    char fp[PROTO_FP_LEN + 1]; // of the pinned cert, matched against mDNS TXT "fp"
    state_t state;
    esp_websocket_client_handle_t client;
    uint32_t gen;
    bool welcomed;
    size_t backoff_idx;
    int64_t retry_at_ms;
    int64_t connect_deadline_ms;
    const char *retry_reason; // leads the status line while backing off
    int shown_retry_s;
    cand_t cands[MAX_CANDIDATES]; // this pass's daemons, own subnet first
    size_t ncand;
    size_t next_cand; // the next one to try
    bool wrong_cert;  // a candidate in this pass presented a cert other than the pinned one
    int64_t browse_at_ms;
    link_daemon_t daemons[LINK_MAX_DAEMONS]; // the pairable list the UI shows
    size_t ndaemons;
    bool listed;                          // the UI has been given a list, even an empty one
    link_daemon_t pair_with;              // the daemon of the attempt
    bool pair_up;                         // that attempt's socket is connected
    int64_t pair_deadline_ms;             // the attempt, or its question in ST_CONFIRM, ends after this
    pageset_t set;                        // the set arriving on this connection, staged in PSRAM
    bool set_broke;                       // a set broke, so welcome keeps the backoff until a set completes
    uint8_t (*icon_q)[PROTO_ICON_ID_LEN]; // in PSRAM: every icon the applied set names, page on screen first
    size_t n_q;
    size_t q_next;                                      // the next of icon_q to ask for, unless it's cached by then
    bool settle_pending;                                // the set's icon_gets aren't all answered or timed out yet
    size_t n_missed;                                    // ids of the set's icon_gets that timed out unanswered
    uint8_t want[PROTO_MAX_BUTTONS][PROTO_ICON_ID_LEN]; // the outstanding icon_get's ids not yet received
    size_t n_want;
    int64_t want_deadline_ms; // the outstanding icon_get is given up after this; each of its frames moves it on
    char *icon_get_buf;       // in PSRAM, so a 40-id icon_get stays off the link task stack
} s;

// Read by other tasks. The id is written once, before the first hello, and never changes.
static char s_device_id[PROTO_DEVICE_ID_LEN];

// Reassembly state and connect measurements, touched only by the websocket event handler, which
// the client runs under its lock.
static char *s_rx;
static int s_rx_len;
static volatile bool s_binary_ok; // set before a client starts: icon frames are taken only from a pinned wss peer
static int64_t s_connect_us;
static bool s_heap_watch;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void set_status(const char *fmt, ...)
{
    char text[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    ESP_LOGI(TAG, "status: %s", text);
    if (s.cbs.on_status) {
        s.cbs.on_status(text);
    }
}

static bool post_wait(const link_ev_t *ev, TickType_t wait)
{
    if (xQueueSend(s.queue, ev, wait) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full, dropped event %d", ev->type);
        return false;
    }
    return true;
}

static bool post(const link_ev_t *ev)
{
    return post_wait(ev, 0);
}

/* ---- Websocket event handler: runs on the client's task, so it only posts. ---- */

static void on_ws_data(const esp_websocket_event_data_t *d, uint32_t gen)
{
    // PING and PONG payloads arrive as DATA too. Fragmented messages (a first frame without FIN, and
    // its continuations with op_code 0) are dropped on purpose, since the protocol uses single frames.
    bool binary = d->op_code == WS_OP_BINARY;
    if (d->op_code != WS_OP_TEXT && !binary) {
        return;
    }
    if (!d->fin) {
        if (d->payload_offset == 0) {
            ESP_LOGW(TAG, "fragmented message dropped");
        }
        return;
    }
    if (binary && !s_binary_ok) {
        // The plain pairing socket has no authentication, so it gets no big allocation.
        if (d->payload_offset == 0) {
            ESP_LOGW(TAG, "binary frame of %d bytes refused, this client takes text only", d->payload_len);
        }
        return;
    }
    const int max_len = binary ? PROTO_MAX_ICON_FRAME : PROTO_MAX_MSG;
    link_ev_t bad = {.type = EV_WS_BAD, .gen = gen};
    if (d->payload_len > max_len) {
        if (d->payload_offset == 0) { // later chunks of the same frame need no second close
            ESP_LOGE(TAG, "frame of %d bytes exceeds %d, closing", d->payload_len, max_len);
            post_wait(&bad, pdMS_TO_TICKS(RX_POST_WAIT_MS));
        }
        return;
    }
    if (d->payload_len == 0) {
        return;
    }
    if (d->payload_offset == 0) {
        free(s_rx);
        s_rx = heap_caps_malloc(d->payload_len + 1, MALLOC_CAP_SPIRAM);
        s_rx_len = 0;
    }
    if (!s_rx || d->payload_offset != s_rx_len || s_rx_len + d->data_len > d->payload_len) {
        ESP_LOGE(TAG, "bad chunk: offset %d, have %d, chunk %d, total %d", d->payload_offset, s_rx_len, d->data_len,
                 d->payload_len);
        free(s_rx);
        s_rx = NULL;
        post_wait(&bad, pdMS_TO_TICKS(RX_POST_WAIT_MS));
        return;
    }
    memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
    s_rx_len += d->data_len;
    if (s_rx_len == d->payload_len) {
        s_rx[s_rx_len] = '\0';
        link_ev_t ev = {
            .type = EV_WS_MSG, .gen = gen, .msg = {.data = s_rx, .len = (size_t)s_rx_len, .binary = binary}};
        // Wait for room, so a burst of icon frames slows the daemon down instead of losing icons.
        if (!post_wait(&ev, pdMS_TO_TICKS(RX_POST_WAIT_MS))) {
            free(s_rx);
        }
        s_rx = NULL;
    }
}

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: internal free %u min %u, PSRAM free %u min %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

static void heap_watch_stop(void)
{
    if (s_heap_watch) {
        s_heap_watch = false;
        ESP_ERROR_CHECK_WITHOUT_ABORT(heap_caps_monitor_local_minimum_free_size_stop());
    }
}

// A connect that failed because the daemon's cert is not the pinned one. esp-tls passes on the
// mbedTLS error (negated) but not the verify flags, which it reads only after a handshake.
static bool cert_rejected(const esp_websocket_event_data_t *d)
{
    int err = d->error_handle.esp_tls_stack_err;
    return d->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT &&
           (err == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || err == -MBEDTLS_ERR_SSL_BAD_CERTIFICATE);
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    const esp_websocket_event_data_t *d = data;
    link_ev_t ev = {.gen = gen};
    switch (id) {
    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        log_heap("before connect");
        // From here the minimum is this attempt's own, so it shows the handshake's peak.
        s_heap_watch = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
        s_connect_us = esp_timer_get_time();
        break;
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "TCP + TLS + WebSocket upgrade took %d ms, websocket task stack %u bytes unused",
                 (int)((esp_timer_get_time() - s_connect_us) / 1000), (unsigned)uxTaskGetStackHighWaterMark(NULL));
        log_heap("session up");
        heap_watch_stop();
        ev.type = EV_WS_UP;
        post(&ev);
        break;
    case WEBSOCKET_EVENT_DATA:
        on_ws_data(d, gen);
        break;
    // ERROR is always followed by DISCONNECTED or CLOSED, and only DISCONNECTED carries the TLS
    // error, so ERROR is left to the client's own log. FINISH covers a task that ends without
    // either.
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_FINISH:
        heap_watch_stop();
        free(s_rx);
        s_rx = NULL;
        ev.type = cert_rejected(d) ? EV_WS_BAD_CERT : EV_WS_DOWN;
        post(&ev);
        break;
    default:
        break;
    }
}

/* ---- Wi-Fi and IP events ---- */

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    link_ev_t ev = {.type = base == IP_EVENT ? EV_GOT_IP : EV_WIFI_DOWN};
    post(&ev);
}

/* ---- mDNS ---- */

static bool same_subnet(uint32_t a, uint32_t b, uint32_t mask)
{
    return (a & mask) == (b & mask);
}

// What is left of a pass's follow-up budget, capped at one query's timeout. 0 when spent.
static uint32_t budget_left(int64_t end_ms)
{
    int64_t left = end_ms - now_ms();
    return left <= 0 ? 0 : left < DISCOVER_TIMEOUT_MS ? (uint32_t)left : DISCOVER_TIMEOUT_MS;
}

// The TXT of `r`: its own, since avahi sends TXT along with the PTR answer, or else a follow-up
// query's. Free *queried with mdns_query_results_free.
static const mdns_result_t *txt_of(const mdns_result_t *r, uint32_t timeout_ms, mdns_result_t **queried)
{
    *queried = NULL;
    if (r->txt_count == 0 && r->instance_name && timeout_ms > 0 &&
        mdns_query_txt(r->instance_name, SERVICE, SERVICE_PROTO, timeout_ms, queried) == ESP_OK && *queried) {
        return *queried;
    }
    return r;
}

// The value of TXT `key`, or NULL. *len gets its length.
static const char *txt_get(const mdns_result_t *t, const char *key, size_t *len)
{
    for (size_t i = 0; i < t->txt_count; i++) {
        if (t->txt[i].key && strcasecmp(t->txt[i].key, key) == 0 && t->txt[i].value) {
            *len = t->txt_value_len[i];
            return t->txt[i].value;
        }
    }
    return NULL;
}

// Up to `max` IPv4 addresses of `r`: from the PTR answer, or else an A query.
static size_t addrs_of(const mdns_result_t *r, uint32_t timeout_ms, esp_ip4_addr_t *out, size_t max)
{
    size_t n = 0;
    for (const mdns_ip_addr_t *a = r->addr; a && n < max; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            out[n++] = a->addr.u_addr.ip4;
        }
    }
    if (n == 0 && r->hostname && timeout_ms > 0) {
        ESP_LOGI(TAG, "%s has no address in the PTR answer, querying A", r->hostname);
        if (mdns_query_a(r->hostname, timeout_ms, &out[0]) == ESP_OK) {
            n = 1;
        }
    }
    return n;
}

// Whether an instance advertises the pinned daemon's fp. *mismatch is set when the TXT was read
// and its fp is absent or different, as opposed to not being read at all.
static bool fp_matches(const mdns_result_t *r, uint32_t timeout_ms, bool *mismatch)
{
    const char *name = r->instance_name ? r->instance_name : "?";
    mdns_result_t *queried;
    const mdns_result_t *t = txt_of(r, timeout_ms, &queried);
    size_t fp_len = 0;
    const char *fp = txt_get(t, "fp", &fp_len);
    bool match = fp && fp_len == PROTO_FP_LEN && memcmp(fp, s.fp, PROTO_FP_LEN) == 0;
    if (!fp) {
        ESP_LOGI(TAG, "skipping %s: no fp in TXT", name);
    } else if (!match) {
        ESP_LOGI(TAG, "skipping %s: fp %.*s is not the pinned %s", name, (int)fp_len, fp, s.fp);
    }
    if (t->txt_count > 0 && !match) {
        *mismatch = true;
    }
    if (queried) {
        mdns_query_results_free(queried);
    }
    return match;
}

// Paired: browse for the pinned daemon and fill s.cands with every address of every instance
// whose fp matches, own subnet first. The TXT and A follow-ups share DISCOVER_BUDGET_MS, so junk
// instances cannot hold the link task for long. Sets *mismatch when instances were seen but none
// matched the pinned fp.
static void discover(bool *mismatch)
{
    s.ncand = 0;
    s.next_cand = 0;
    s.wrong_cert = false;
    *mismatch = false;

    esp_netif_ip_info_t ours = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_get_ip_info(sta, &ours);
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(SERVICE, SERVICE_PROTO, DISCOVER_TIMEOUT_MS, MAX_CANDIDATES, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr: %s", esp_err_to_name(err));
        return;
    }

    cand_t found[MAX_CANDIDATES];
    size_t n = 0;
    bool any_match = false;
    bool any_mismatch = false;
    int64_t budget_end_ms = now_ms() + DISCOVER_BUDGET_MS;
    for (const mdns_result_t *r = results; r; r = r->next) {
        uint32_t left = budget_left(budget_end_ms);
        if (left == 0) {
            ESP_LOGW(TAG, "discovery budget of %d ms spent, ignoring the remaining instances", DISCOVER_BUDGET_MS);
            break;
        }
        if (!fp_matches(r, left, &any_mismatch)) {
            continue;
        }
        any_match = true;
        esp_ip4_addr_t addrs[4];
        size_t na = addrs_of(r, budget_left(budget_end_ms), addrs, 4);
        for (size_t i = 0; i < na && n < MAX_CANDIDATES; i++) {
            found[n].ip = addrs[i];
            found[n].port = r->port;
            found[n].local = same_subnet(addrs[i].addr, ours.ip.addr, ours.netmask.addr);
            ESP_LOGI(TAG, "candidate %s at " IPSTR ":%u%s", r->hostname ? r->hostname : "?", IP2STR(&addrs[i]), r->port,
                     found[n].local ? " (our subnet)" : "");
            n++;
        }
    }
    mdns_query_results_free(results);

    for (int want_local = 1; want_local >= 0; want_local--) {
        for (size_t i = 0; i < n; i++) {
            if (found[i].local == want_local) {
                s.cands[s.ncand++] = found[i];
            }
        }
    }
    *mismatch = any_mismatch && !any_match;
}

// TXT "pair": the pairing port, 1 to 5 decimal digits in 1..65535. 0 when absent or invalid.
static uint16_t pair_port(const mdns_result_t *t)
{
    size_t len = 0;
    const char *value = txt_get(t, "pair", &len);
    if (!value || len == 0 || len > 5) {
        return 0;
    }
    uint32_t port = 0;
    for (size_t i = 0; i < len; i++) {
        if (value[i] < '0' || value[i] > '9') {
            return 0;
        }
        port = port * 10 + (uint32_t)(value[i] - '0');
    }
    return port <= 65535 ? (uint16_t)port : 0;
}

// Unpaired: list the protocol-v1 daemons open for pairing, and hand the list to the UI when it
// changes.
static void browse(void)
{
    link_daemon_t found[LINK_MAX_DAEMONS];
    size_t n = 0;
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(SERVICE, SERVICE_PROTO, DISCOVER_TIMEOUT_MS, MAX_CANDIDATES, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr: %s", esp_err_to_name(err));
    }
    int64_t budget_end_ms = now_ms() + DISCOVER_BUDGET_MS;
    for (const mdns_result_t *r = results; r && n < LINK_MAX_DAEMONS; r = r->next) {
        uint32_t left = budget_left(budget_end_ms);
        if (left == 0) {
            break;
        }
        mdns_result_t *queried;
        const mdns_result_t *t = txt_of(r, left, &queried);
        size_t v_len = 0;
        const char *v = txt_get(t, "v", &v_len);
        uint16_t port = (!v || (v_len == 1 && v[0] == '1')) ? pair_port(t) : 0;
        if (queried) {
            mdns_query_results_free(queried);
        }
        esp_ip4_addr_t ip;
        if (port == 0 || addrs_of(r, budget_left(budget_end_ms), &ip, 1) == 0) {
            continue;
        }
        link_daemon_t *d = &found[n++];
        memset(d, 0, sizeof(*d)); // zeroed padding keeps the memcmp below meaningful
        strlcpy(d->name, r->instance_name ? r->instance_name : "?", sizeof(d->name));
        d->ip = ip;
        d->port = port;
    }
    if (results) {
        mdns_query_results_free(results);
    }

    if (!s.listed || n != s.ndaemons || memcmp(found, s.daemons, n * sizeof(found[0])) != 0) {
        memcpy(s.daemons, found, n * sizeof(found[0]));
        s.ndaemons = n;
        s.listed = true;
        ESP_LOGI(TAG, "%u daemon(s) open for pairing", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "  %s at " IPSTR ", pairing port %u", s.daemons[i].name, IP2STR(&s.daemons[i].ip),
                     s.daemons[i].port);
        }
        if (s.cbs.on_daemons) {
            s.cbs.on_daemons(s.daemons, n);
        }
    }
    s.browse_at_ms = now_ms() + BROWSE_INTERVAL_MS;
}

/* ---- Session control (link task only) ---- */

static void report_retry(void)
{
    int64_t left = s.retry_at_ms - now_ms();
    int secs = left > 0 ? (int)((left + 999) / 1000) : 0;
    if (secs != s.shown_retry_s) {
        s.shown_retry_s = secs;
        set_status("%s, retry in %d s", s.retry_reason, secs);
    }
}

// Back off before the next attempt, with `reason` leading the status line until then.
static void schedule_retry(const char *reason)
{
    s.retry_reason = reason;
    uint32_t base_ms = BACKOFF_S[s.backoff_idx] * 1000;
    if (s.backoff_idx < sizeof(BACKOFF_S) / sizeof(BACKOFF_S[0]) - 1) {
        s.backoff_idx++;
    }
    uint32_t pct = 80 + esp_random() % 41; // 80..120
    uint32_t wait_ms = (uint32_t)((uint64_t)base_ms * pct / 100);
    s.retry_at_ms = now_ms() + wait_ms;
    s.state = ST_BACKOFF;
    s.shown_retry_s = -1;
    ESP_LOGI(TAG, "retry in %u ms (base %u s)", (unsigned)wait_ms, (unsigned)base_ms / 1000);
    report_retry();
}

// Wi-Fi is up: retry the paired daemon now, or look for daemons to pair with.
static void resume(void)
{
    s.state = s.paired ? ST_BACKOFF : ST_BROWSE;
    s.retry_at_ms = now_ms();
    s.browse_at_ms = now_ms();
}

// Destroy the client (never from its own handler) and fence off its late events.
static void drop_client(void)
{
    s.gen++;
    if (s.client) {
        esp_websocket_client_destroy(s.client);
        s.client = NULL;
    }
    s.welcomed = false;
    // Sets, revs and icon requests belong to one connection. The page on screen (in the model) and set_broke don't.
    pageset_reset(&s.set);
    s.n_q = 0;
    s.q_next = 0;
    s.settle_pending = false;
    s.n_missed = 0;
    s.n_want = 0;
}

// Drop the session and back off, showing `reason` on the status line.
static void lost_as(const char *why, const char *reason)
{
    diag_count(DIAG_LINK_DROP, why);
    ESP_LOGW(TAG, "link lost: %s", why);
    drop_client();
    if (s.cbs.on_down) {
        s.cbs.on_down();
    }
    schedule_retry(reason);
}

static void lost(const char *why)
{
    lost_as(why, "offline");
}

// Start a client for `uri`. `cert_pem` pins the daemon on wss and is NULL on plain ws. The
// pinned cert is the only trust anchor and the chain check stays on. The daemon's IP changes,
// so the name check is off, which also turns off SNI.
static esp_err_t open_client(const char *uri, const char *cert_pem)
{
    s.gen++;
    s_binary_ok = cert_pem != NULL;
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .cert_pem = cert_pem,
        .skip_cert_common_name_check = cert_pem != NULL,
        .disable_auto_reconnect = true,
        .ping_interval_sec = PING_INTERVAL_S,
        .pingpong_timeout_sec = PINGPONG_TIMEOUT_S,
        .buffer_size = PROTO_MAX_MSG,
        .task_stack = WS_TASK_STACK,
        .network_timeout_ms = 5000,
        // A peer that vanishes mid-frame blocks the websocket task, so drop it at the TCP level after
        // about 20 s.
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3,
    };
    s.client = esp_websocket_client_init(&cfg);
    esp_err_t err =
        s.client ? esp_websocket_register_events(s.client, WEBSOCKET_EVENT_ANY, ws_handler, (void *)(uintptr_t)s.gen)
                 : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = esp_websocket_client_start(s.client);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "websocket start: %s", esp_err_to_name(err));
    }
    return err;
}

static void candidate_failed(const char *why);

static void start_client(const cand_t *c)
{
    char uri[48];
    snprintf(uri, sizeof(uri), "wss://" IPSTR ":%u/", IP2STR(&c->ip), c->port);
    if (open_client(uri, s.pairing->cert_pem) != ESP_OK) {
        candidate_failed("websocket start failed");
        return;
    }
    s.state = ST_CONNECTING;
    s.connect_deadline_ms = now_ms() + CONNECT_TIMEOUT_MS;
    set_status("connecting to " IPSTR, IP2STR(&c->ip));
}

// A candidate failed before its session came up. Try the next one at once, and back off only
// after every candidate has failed. Any LAN peer can advertise the public fp, so a cert mismatch
// from a candidate that matched the fp reads as a wrong cert, not as a changed identity.
static void candidate_failed(const char *why)
{
    if (s.next_cand < s.ncand) {
        ESP_LOGW(TAG, "candidate failed (%s), trying the next of %u", why, (unsigned)s.ncand);
        drop_client();
        start_client(&s.cands[s.next_cand++]);
        return;
    }
    lost_as(why, s.wrong_cert ? "wrong cert from daemon" : "offline");
}

static void attempt(void)
{
    set_status("searching");
    bool mismatch;
    discover(&mismatch);
    if (s.ncand == 0) {
        ESP_LOGW(TAG, "pinned daemon not found%s", mismatch ? ": instances seen, none with the pinned fp" : "");
        schedule_retry(mismatch ? "daemon identity changed" : "offline");
        return;
    }
    start_client(&s.cands[s.next_cand++]);
}

// "wd-" plus the C6's STA MAC. esp_wifi_get_mac asks the C6 for its real MAC;
// esp_read_mac(ESP_MAC_WIFI_STA) returns a placeholder on the P4, which has no Wi-Fi radio.
static esp_err_t device_id(char out[PROTO_DEVICE_ID_LEN])
{
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mac: %s", esp_err_to_name(err));
        return err;
    }
    proto_device_id(out, mac);
    if (!s_device_id[0]) {
        memcpy(s_device_id, out, PROTO_DEVICE_ID_LEN);
    }
    return ESP_OK;
}

static void pair_failed(const char *why);

// Send one text frame. `what` stands in for the JSON in the log when the JSON holds a secret.
static void send_text(const char *json, size_t len, const char *what)
{
    ESP_LOGI(TAG, "tx: %s", what ? what : json);
    if (esp_websocket_client_send_text(s.client, json, (int)len, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) < 0) {
        if (s.state == ST_PAIRING) {
            pair_failed("send failed");
        } else {
            lost("send failed");
        }
    }
}

static void send_hello(void)
{
    char device[PROTO_DEVICE_ID_LEN];
    if (device_id(device) != ESP_OK) {
        lost("no MAC");
        return;
    }
    // Only names this build fully supports: "fonts" means ui.c draws every font id in font_list.h, and "pages" that
    // pageset.c assembles sets of up to PROTO_MAX_PAGES and ui.c lays out the paged grid, and "status_bar" that ui.c
    // reads `stats` frames, and "feed" that ui.c shows the -1 page from `feed` frames and dismisses with feed_dismiss.
    // The hello must stay under 256 bytes: test_proto.c pins its size for this list, and for the list with "sleep".
    static const char *const caps[] = {"fonts", "pages", "status_bar", "feed"};
    char buf[256];
    size_t len = proto_build_hello(buf, sizeof(buf), device, s.pairing->token, esp_app_get_description()->version,
                                   DISPLAY_H_RES, DISPLAY_V_RES, caps, sizeof(caps) / sizeof(caps[0]));
    if (len == 0) {
        lost("hello does not fit");
        return;
    }
    send_text(buf, len, "hello (token redacted)");
}

// Send the pending crash or reset report, if any. It goes out once per session and stays pending
// until the daemon acks it.
static void send_report(void)
{
    const proto_report_t *r = crash_pending();
    if (!r) {
        return;
    }
    char buf[512];
    size_t len = proto_build_report(buf, sizeof(buf), r);
    if (len == 0) {
        ESP_LOGE(TAG, "report %s is invalid or does not fit, dropped", r->id);
        crash_erase(); // the daemon would ack and discard a malformed one anyway
        return;
    }
    send_text(buf, len, NULL);
}

/* ---- Pairing mode ---- */

// Enter paired or unpaired mode and tell the UI. Paired, s.pairing holds the blob.
static void set_paired(bool paired)
{
    bool was_paired = s.paired;
    s.paired = paired;
    logcap_set_token(paired ? s.pairing->token : NULL); // so the log ring redacts it
    if (paired) {
        if (proto_fp(s.pairing->cert_pem, s.fp) != ESP_OK) {
            s.fp[0] = '\0'; // matches no instance, so the status will say why
        }
        ESP_LOGI(TAG, "paired with %s, pinned daemon fp %s", s.pairing->name, s.fp);
    } else {
        memset(s.pairing, 0, sizeof(*s.pairing));
        s.fp[0] = '\0';
        s.listed = false; // the next browse reports even an empty list
    }
    if (s.cbs.on_paired) {
        s.cbs.on_paired(paired);
    }
    if (was_paired && !paired) {
        // The UI has dropped the grid that showed the set. LVGL first, since dropping an icon takes its lock.
        lvgl_port_lock(0);
        model_lock();
        model_clear();
        model_unlock();
        lvgl_port_unlock();
    }
}

// Drop the pairing from RAM, because the daemon refused the token or the user asked, and list
// again. The caller erases the blob first.
static void unpair(const char *reason)
{
    ESP_LOGW(TAG, "unpairing: %s", reason);
    drop_client();
    if (s.cbs.on_down) {
        s.cbs.on_down();
    }
    set_paired(false);
    set_status("%s", reason);
    if (s.state != ST_NO_IP) {
        s.state = ST_BROWSE;
        s.browse_at_ms = now_ms();
    }
}

static void start_pairing(const link_daemon_t *d)
{
    char uri[48];
    snprintf(uri, sizeof(uri), "ws://" IPSTR ":%u/", IP2STR(&d->ip), d->port);
    s.pair_with = *d;
    s.pair_up = false;
    s.state = ST_PAIRING;
    s.connect_deadline_ms = now_ms() + CONNECT_TIMEOUT_MS;
    s.pair_deadline_ms = now_ms() + PAIR_TIMEOUT_MS;
    set_status("pairing with %s", d->name);
    if (open_client(uri, NULL) != ESP_OK) {
        pair_failed("could not start");
    }
}

// End the attempt, wipe its token, cert, and name, and go back to the list.
static void end_attempt(void)
{
    drop_client();
    pairing_end(s.session);
    if (s.cbs.on_pair_code) {
        s.cbs.on_pair_code(NULL, NULL);
    }
    s.state = ST_BROWSE;
    s.browse_at_ms = now_ms();
}

// End the attempt with `why` on the status line.
static void pair_failed(const char *why)
{
    char reason[sizeof(s.session->error)];
    strlcpy(reason, why, sizeof(reason)); // `why` may live in the session wiped below
    ESP_LOGW(TAG, "pairing with %s failed: %s", s.pair_with.name, reason);
    end_attempt();
    set_status("pairing failed: %s", reason);
}

// The person said No to the question, or did not answer it.
static void pair_declined(const char *why)
{
    ESP_LOGW(TAG, "pairing with %s cancelled: %s", s.pair_with.name, why);
    end_attempt();
    set_status("pairing cancelled");
}

// Anyone on the plain pairing socket can send pair_result ok, so it proves nothing about the
// daemon. Keep the result in RAM until the person confirms on the device that the desktop shows
// the pairing. An honest daemon has already stored its side.
static void ask_confirm(void)
{
    drop_client();
    s.state = ST_CONFIRM;
    s.pair_deadline_ms = now_ms() + CONFIRM_TIMEOUT_MS;
    ESP_LOGI(TAG, "%s says paired, asking the person to confirm", s.session->result.name);
    set_status("confirm the pairing below");
    if (s.cbs.on_pair_confirm) {
        s.cbs.on_pair_confirm(s.session->device, s.session->result.name);
    }
}

// The person said Yes: save the pairing and connect.
static void pair_done(void)
{
    if (pairing_save(&s.session->result) != ESP_OK) {
        pair_failed("could not save the pairing");
        return;
    }
    *s.pairing = s.session->result;
    pairing_end(s.session);
    if (s.cbs.on_pair_code) {
        s.cbs.on_pair_code(NULL, NULL);
    }
    ESP_LOGI(TAG, "pairing saved, link task stack %u bytes unused", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    set_paired(true);
    set_status("paired with %s", s.pairing->name);
    s.backoff_idx = 0;
    s.state = ST_BACKOFF;
    s.retry_at_ms = now_ms();
}

static void pair_connected(void)
{
    s.pair_up = true;
    char device[PROTO_DEVICE_ID_LEN];
    if (device_id(device) != ESP_OK) {
        pair_failed("no MAC");
        return;
    }
    char buf[160];
    size_t len = pairing_begin(s.session, device, buf, sizeof(buf));
    if (len == 0) {
        pair_failed(s.session->error);
        return;
    }
    send_text(buf, len, NULL);
}

static void handle_pair_msg(const char *json, size_t len)
{
    ESP_LOGI(TAG, "rx: %.*s", (int)(len > 160 ? 160 : len), json);
    proto_msg_t *m = s.msg;
    if (proto_parse(json, len, m) != ESP_OK) {
        pair_failed("bad message from the daemon");
        return;
    }
    if (m->type == PROTO_MSG_UNKNOWN) {
        ESP_LOGW(TAG, "unknown message type \"%s\", ignored", m->unknown_t);
        return;
    }
    char reply[160];
    size_t reply_len;
    switch (pairing_feed(s.session, m, reply, sizeof(reply), &reply_len)) {
    case PAIRING_REPLY:
        send_text(reply, reply_len, NULL);
        break;
    case PAIRING_CODE:
        // Every input to the code crossed the wire in the clear, so the code is no secret.
        ESP_LOGI(TAG, "pairing code %s", s.session->code);
        if (s.cbs.on_pair_code) {
            s.cbs.on_pair_code(s.session->code, s.session->result.name);
        }
        set_status("type the code into watchdeck on %s", s.session->result.name);
        break;
    case PAIRING_DONE:
        ask_confirm();
        break;
    case PAIRING_FAILED:
        pair_failed(s.session->error);
        break;
    }
}

/* ---- Paired session ---- */

#define ID4(id) (id)[0], (id)[1], (id)[2], (id)[3] // enough of an icon id for the log

static bool has_id(const uint8_t ids[][PROTO_ICON_ID_LEN], size_t n, const uint8_t id[PROTO_ICON_ID_LEN])
{
    for (size_t i = 0; i < n; i++) {
        if (memcmp(ids[i], id, PROTO_ICON_ID_LEN) == 0) {
            return true;
        }
    }
    return false;
}

// Send the next icon_get: up to 40 of the set's icons, in icon_q order, that aren't cached by now. This replaces any
// request still outstanding, whose frames may still come. With nothing left to ask for, the set's icons have settled.
static void request_icons(void)
{
    s.n_want = 0;
    model_lock();
    while (s.q_next < s.n_q && s.n_want < PROTO_MAX_BUTTONS) {
        const uint8_t *id = s.icon_q[s.q_next++];
        if (!icons_find(id)) { // a frame of an earlier request may have brought it
            memcpy(s.want[s.n_want++], id, PROTO_ICON_ID_LEN);
        }
    }
    model_unlock();
    if (s.n_want == 0) {
        if (s.settle_pending && s.n_missed > 0) {
            // Frames may still be coming, so this is no settled point to sample memory at.
            s.settle_pending = false;
            ESP_LOGW(TAG, "icons gave up waiting (%u missing)", (unsigned)s.n_missed);
        } else if (s.settle_pending) {
            s.settle_pending = false;
            ESP_LOGI(TAG, "icons settled, link task stack %u bytes unused",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (s.cbs.on_icons_settled) {
                s.cbs.on_icons_settled();
            }
        }
        return;
    }
    size_t len = proto_build_icon_get(s.icon_get_buf, ICON_GET_BUF_LEN, s.want, s.n_want);
    if (len == 0) {
        ESP_LOGE(TAG, "icon_get does not fit");
        s.n_want = 0;
        return;
    }
    ESP_LOGI(TAG, "requesting %u icon(s), %u more of the set to go", (unsigned)s.n_want, (unsigned)(s.n_q - s.q_next));
    s.want_deadline_ms = now_ms() + ICON_GET_TIMEOUT_MS;
    send_text(s.icon_get_buf, len, NULL);
}

// One binary frame. Returns true if the cache took the buffer, else the caller frees it.
static bool handle_icon_frame(uint8_t *frame, size_t len)
{
    proto_icon_t icon;
    if (!s.welcomed) {
        ESP_LOGW(TAG, "icon before welcome, ignored");
        return false;
    }
    if (proto_parse_icon(frame, len, &icon) != ESP_OK) {
        return false; // proto_parse_icon says why
    }
    // Any icon the set names is taken, so a frame of a request a new set replaced isn't wasted.
    model_lock();
    const bool already = icons_find(icon.id) != NULL;
    const bool named = !already && has_id(s.icon_q, s.n_q, icon.id);
    const esp_err_t err = named ? icons_add(&icon, frame) : ESP_FAIL;
    const lv_image_dsc_t *dsc = err == ESP_OK ? icons_find(icon.id) : NULL;
    model_unlock();
    if (already) {
        ESP_LOGI(TAG, "icon %02x%02x%02x%02x already cached, ignored", ID4(icon.id));
        return false;
    }
    if (!named) {
        ESP_LOGW(TAG, "icon %02x%02x%02x%02x was not requested, dropped", ID4(icon.id));
        return false;
    }
    if (err != ESP_OK) {
        return false; // icons_add says why
    }
    size_t i = 0;
    while (i < s.n_want && memcmp(s.want[i], icon.id, PROTO_ICON_ID_LEN) != 0) {
        i++;
    }
    const bool outstanding = i < s.n_want;
    if (outstanding) {
        memcpy(s.want[i], s.want[--s.n_want], PROTO_ICON_ID_LEN);
        s.want_deadline_ms = now_ms() + ICON_GET_TIMEOUT_MS; // frames still coming, so the request is alive
    }
    ESP_LOGI(TAG, "icon %02x%02x%02x%02x %ux%u received, validated and cached, %u still wanted", ID4(icon.id), icon.w,
             icon.h, (unsigned)s.n_want);
    if (s.cbs.on_icon) {
        s.cbs.on_icon(icon.id, dsc); // only this task frees icons, so dsc outlives the lock
    }
    if (outstanding && s.n_want == 0) {
        request_icons(); // the request is answered, so the next goes out
    }
    return true;
}

// Drop the link over a broken set, so the next session sends a complete one. The backoff grows until a set completes.
static void set_broken(const char *why)
{
    diag_count(DIAG_SET_BROKEN, why);
    ESP_LOGW(TAG, "layout set broken: %s", why);
    s.set_broke = true;
    lost_as("layout set broken", "layout set broken");
}

// A set is complete. Apply it, have the UI build the page on screen, ack the set, then fetch the icons it lacks.
static void apply_set(void)
{
    model_lock();
    model_apply(&s.set);
    model_unlock();
    if (s.set_broke) {
        s.set_broke = false;
        s.backoff_idx = 0; // the welcome before this set left it as it was
    }
    if (s.cbs.on_set) {
        s.cbs.on_set();
    }
    // Before any icon_get, so the ack doesn't wait behind icon frames. Pictures still to come fill in later, so the
    // ack means the buttons are on screen.
    char ack[48];
    size_t ack_len = proto_build_layout_ack(ack, sizeof(ack), s.set.rev);
    if (ack_len > 0) {
        send_text(ack, ack_len, NULL);
    }
    if (!s.welcomed) {
        return; // the send failed and dropped the link
    }

    // The UI has replaced the widgets of the last set, so free the icons no page names any more. LVGL first, since
    // dropping an icon takes its lock.
    lvgl_port_lock(0);
    model_lock();
    const size_t dropped = icons_retain(model_page(0), model_pages());
    model_unlock();
    lvgl_port_unlock();

    // Every icon the set names, once each: the page on screen first, then the others in order.
    size_t cached = 0;
    s.n_q = 0;
    s.q_next = 0;
    model_lock();
    const uint8_t n_pages = model_pages();
    const uint8_t shown = model_shown();
    for (uint8_t k = 0; k < n_pages; k++) {
        const uint8_t p = k == 0 ? shown : (k - 1 < shown ? k - 1 : k);
        const proto_layout_t *page = model_page(p);
        for (size_t b = 0; b < page->n_buttons; b++) {
            const uint8_t *id = page->buttons[b].icon;
            if (!page->buttons[b].has_icon || has_id(s.icon_q, s.n_q, id)) {
                continue;
            }
            memcpy(s.icon_q[s.n_q++], id, PROTO_ICON_ID_LEN);
            cached += icons_find(id) != NULL;
        }
    }
    model_unlock();
    ESP_LOGI(TAG, "set names %u icon(s): %u cached, %u to request, %u dropped", (unsigned)s.n_q, (unsigned)cached,
             (unsigned)(s.n_q - cached), (unsigned)dropped);
    s.settle_pending = true;
    s.n_missed = 0;
    request_icons(); // what is left of the last set's requests is cancelled
}

static void handle_layout(const proto_layout_t *l)
{
    const char *why;
    const pageset_result_t r = pageset_add(&s.set, l, now_ms(), &why);
    if (r == PAGESET_BROKEN) {
        set_broken(why);
        return;
    }
    if (why) {
        ESP_LOGW(TAG, "layout rev %u: %s", (unsigned)l->rev, why);
    }
    if (r == PAGESET_PENDING) {
        ESP_LOGI(TAG, "layout rev %u: page %u of %u in", (unsigned)l->rev, l->page + 1, l->pages);
        return;
    }
    apply_set();
}

static void handle_msg(const char *json, size_t len)
{
    proto_msg_t *m = s.msg;
    const esp_err_t parsed = proto_parse(json, len, m);
    // A stats frame comes every second, so it gets a debug line below instead of an INFO one.
    if (parsed != ESP_OK || m->type != PROTO_MSG_STATS) {
        ESP_LOGI(TAG, "rx: %.*s", (int)(len > 160 ? 160 : len), json);
    }
    if (parsed != ESP_OK) {
        if (pageset_arriving(&s.set)) {
            set_broken("a page that won't parse"); // nothing comes between the pages of a set
        } else {
            ESP_LOGW(TAG, "dropped unparsable message");
        }
        return;
    }
    switch (m->type) {
    case PROTO_MSG_WELCOME:
        s.welcomed = true;
        if (s.set_broke) {
            ESP_LOGW(TAG, "backoff kept until a layout set completes");
        } else {
            s.backoff_idx = 0;
        }
        // A new session resends every tile after its first set.
        model_lock();
        model_clear_tiles();
        model_unlock();
        if (s.cbs.on_welcome) {
            s.cbs.on_welcome(m->welcome_name);
        }
        set_status("connected to %s", m->welcome_name);
        send_report();
        crash_test_arm();
        break;
    case PROTO_MSG_REPORT_ACK:
        if (!s.welcomed) {
            ESP_LOGW(TAG, "message before welcome, ignored");
        } else if (!crash_ack(m->report_ack_id)) {
            ESP_LOGW(TAG, "report_ack for an unknown report, ignored");
        }
        break;
    case PROTO_MSG_ERROR:
        ESP_LOGE(TAG, "daemon error %s: %s", m->error.code, m->error.msg);
        // Only the pinned daemon can say this, over the TLS session, so it is safe to obey.
        if (strcmp(m->error.code, "unauthorized") == 0) {
            // pairing_erase logs its own failure. The daemon refuses this token, so a blob that
            // outlives a failed erase is refused again at the next connect.
            pairing_erase();
            crash_erase(); // a dump may hold the token
            unpair("the desktop forgot this device, pair again");
        } else {
            set_status("daemon error: %s", m->error.code);
        }
        break;
    case PROTO_MSG_LAYOUT:
    case PROTO_MSG_RESULT:
    case PROTO_MSG_TILE:
        if (!s.welcomed) {
            ESP_LOGW(TAG, "message before welcome, ignored");
        } else if (m->type == PROTO_MSG_LAYOUT) {
            handle_layout(&m->layout);
        } else if (m->type == PROTO_MSG_RESULT && s.cbs.on_result) {
            s.cbs.on_result(&m->result);
        } else if (m->type == PROTO_MSG_TILE) {
            model_lock();
            const bool stored = model_set_tile(&m->tile);
            model_unlock();
            if (stored && s.cbs.on_tile) {
                s.cbs.on_tile(&m->tile);
            }
        }
        break;
    case PROTO_MSG_STATS:
        if (!s.welcomed) {
            ESP_LOGW(TAG, "message before welcome, ignored");
        } else {
            ESP_LOGD(TAG, "stats: time %llu cpu %s%u mem %s%llu/%llu net %s%llu/%llu",
                     (unsigned long long)m->stats.time, m->stats.has_cpu ? "" : "(none) ",
                     m->stats.has_cpu ? m->stats.cpu : 0u, m->stats.has_mem ? "" : "(none) ",
                     (unsigned long long)m->stats.mem_used, (unsigned long long)m->stats.mem_total,
                     m->stats.has_net ? "" : "(none) ", (unsigned long long)m->stats.net_rx,
                     (unsigned long long)m->stats.net_tx);
            if (s.cbs.on_stats) {
                s.cbs.on_stats(&m->stats);
            }
        }
        break;
    case PROTO_MSG_FEED:
        if (!s.welcomed) {
            ESP_LOGW(TAG, "message before welcome, ignored");
        } else {
            ESP_LOGI(TAG, "feed: %u source%s, %u card%s, %u note%s", m->feed.sources, m->feed.sources == 1 ? "" : "s",
                     (unsigned)m->feed.n_cards, m->feed.n_cards == 1 ? "" : "s", (unsigned)m->feed.n_notes,
                     m->feed.n_notes == 1 ? "" : "s");
            model_lock();
            model_set_feed(&m->feed);
            model_unlock();
            if (s.cbs.on_feed) {
                s.cbs.on_feed();
            }
        }
        break;
    case PROTO_MSG_UNKNOWN:
        ESP_LOGW(TAG, "unknown message type \"%s\", ignored", m->unknown_t);
        break;
    default:
        ESP_LOGW(TAG, "pairing message on the session, ignored");
        break;
    }
}

static void handle_event(link_ev_t *ev)
{
    bool current = ev->gen == s.gen;
    switch (ev->type) {
    case EV_GOT_IP:
        if (!s.set_broke) { // a broken set keeps the backoff until a set completes
            s.backoff_idx = 0;
        }
        if (s.state == ST_NO_IP || s.state == ST_BACKOFF || s.state == ST_BROWSE) {
            resume();
        }
        break;
    case EV_WIFI_DOWN:
        if (s.state != ST_NO_IP) {
            if (s.state == ST_PAIRING || s.state == ST_CONFIRM) {
                pairing_end(s.session);
                if (s.cbs.on_pair_code) {
                    s.cbs.on_pair_code(NULL, NULL);
                }
            }
            drop_client();
            if (s.cbs.on_down) {
                s.cbs.on_down();
            }
            s.state = ST_NO_IP;
            set_status("offline, waiting for Wi-Fi");
        }
        break;
    case EV_WS_UP:
        if (current && s.state == ST_CONNECTING) {
            s.state = ST_UP;
            s.connect_deadline_ms = now_ms() + CONNECT_TIMEOUT_MS; // for the welcome
            send_hello();
        } else if (current && s.state == ST_PAIRING && !s.pair_up) {
            pair_connected();
        }
        break;
    case EV_WS_DOWN:
        if (current && s.client) {
            if (s.state == ST_CONNECTING) {
                candidate_failed("connection closed");
            } else if (s.state == ST_PAIRING) {
                pair_failed("connection closed");
            } else {
                lost("connection closed");
            }
        }
        break;
    case EV_WS_BAD:
        if (current && s.client) {
            if (s.state == ST_PAIRING) {
                pair_failed("bad frame");
            } else {
                lost("bad frame");
            }
        }
        break;
    case EV_WS_BAD_CERT:
        if (current && s.client && s.state == ST_PAIRING) {
            pair_failed("connection closed"); // plain ws has no cert, so this is just a drop
        } else if (current && s.client) {
            ESP_LOGE(TAG, "the daemon at candidate %u presented a cert other than the pinned one",
                     (unsigned)s.next_cand);
            s.wrong_cert = true;
            candidate_failed("cert rejected");
        }
        break;
    case EV_WS_MSG: {
        bool kept = false;
        if (current && s.state == ST_UP) {
            if (ev->msg.binary) {
                kept = handle_icon_frame((uint8_t *)ev->msg.data, ev->msg.len);
            } else {
                handle_msg(ev->msg.data, ev->msg.len);
            }
        } else if (current && s.state == ST_PAIRING && !ev->msg.binary) {
            handle_pair_msg(ev->msg.data, ev->msg.len);
        }
        if (!kept) {
            free(ev->msg.data);
        }
        break;
    }
    case EV_PRESS:
        if (s.state == ST_UP && s.welcomed) {
            char buf[PROTO_ID_MAX + 64];
            size_t len = proto_build_press(buf, sizeof(buf), ev->press.btn, ev->press.seq);
            if (len > 0) {
                send_text(buf, len, NULL);
            }
        } else {
            ESP_LOGW(TAG, "link down, press %u dropped", (unsigned)ev->press.seq);
        }
        break;
    case EV_FEED_DISMISS:
        if (s.state == ST_UP && s.welcomed) {
            char buf[2 * PROTO_ID_MAX + 32]; // an id of 63 characters may all be escaped
            size_t len = proto_build_feed_dismiss(buf, sizeof(buf), ev->press.btn);
            if (len > 0) {
                send_text(buf, len, NULL);
            } else {
                ESP_LOGW(TAG, "feed dismiss for %s not built, dropped", ev->press.btn);
            }
        } else {
            ESP_LOGW(TAG, "link down, feed dismiss dropped");
        }
        break;
    case EV_PAIR:
        if (!s.paired && s.state == ST_BROWSE) {
            start_pairing(&ev->daemon);
        } else {
            ESP_LOGW(TAG, "pair request ignored: not listing daemons");
        }
        break;
    case EV_PAIR_CANCEL:
        if (s.state == ST_PAIRING) {
            pair_failed("cancelled");
        } else if (s.state == ST_CONFIRM) {
            pair_declined("the person said No");
        }
        break;
    case EV_PAIR_CONFIRM:
        if (s.state == ST_CONFIRM) {
            pair_done();
        }
        break;
    case EV_FORGET:
        if (s.paired) {
            // A blob that survives a failed erase comes back at the next boot, and the daemon
            // still accepts its token, so stay paired and say so.
            esp_err_t err = pairing_erase();
            if (err == ESP_OK) {
                crash_erase(); // a dump may hold the token
                unpair("pairing forgotten");
            } else {
                set_status("forget failed: %s", esp_err_to_name(err));
            }
        }
        break;
    }
}

static void link_task(void *arg)
{
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(1000);
        int64_t due_ms = s.state == ST_BACKOFF ? s.retry_at_ms : s.state == ST_BROWSE ? s.browse_at_ms : -1;
        if (due_ms >= 0) {
            int64_t left = due_ms - now_ms();
            wait = left <= 0 ? 0 : pdMS_TO_TICKS(left < 1000 ? left : 1000) + 1;
        }
        link_ev_t ev;
        if (xQueueReceive(s.queue, &ev, wait) == pdTRUE) {
            handle_event(&ev);
        }
        int64_t now = now_ms();
        switch (s.state) {
        case ST_BACKOFF:
            if (now >= s.retry_at_ms) {
                attempt();
            } else {
                report_retry();
            }
            break;
        case ST_CONNECTING:
            if (now > s.connect_deadline_ms) {
                candidate_failed("connect timeout");
            }
            break;
        case ST_UP:
            if (!s.welcomed && now > s.connect_deadline_ms) {
                candidate_failed("no welcome");
            } else if (pageset_expired(&s.set, now)) {
                set_broken("still incomplete 5 s after its first page");
            } else if (s.n_want > 0 && now > s.want_deadline_ms) {
                diag_count(DIAG_ICON_TIMEOUT, "icon_get unanswered");
                ESP_LOGW(TAG, "icon_get timed out with %u icon(s) not received", (unsigned)s.n_want);
                s.n_missed += s.n_want;
                request_icons();
            }
            break;
        case ST_BROWSE:
            if (now >= s.browse_at_ms) {
                browse();
            }
            break;
        case ST_PAIRING:
            if (!s.pair_up && now > s.connect_deadline_ms) {
                pair_failed("could not connect");
            } else if (now > s.pair_deadline_ms) {
                pair_failed("timed out");
            }
            break;
        case ST_CONFIRM:
            if (now > s.pair_deadline_ms) {
                pair_declined("no answer");
            }
            break;
        default:
            break;
        }
    }
}

esp_err_t link_start(const link_cbs_t *cbs)
{
    ESP_RETURN_ON_FALSE(cbs && !s.queue, ESP_ERR_INVALID_STATE, TAG, "bad start");
    s.cbs = *cbs;
    s.msg = heap_caps_malloc(sizeof(*s.msg), MALLOC_CAP_SPIRAM);
    s.pairing = heap_caps_calloc(1, sizeof(*s.pairing), MALLOC_CAP_SPIRAM);
    s.session = heap_caps_calloc(1, sizeof(*s.session), MALLOC_CAP_SPIRAM);
    s.set.pages = heap_caps_calloc(PROTO_MAX_PAGES, sizeof(*s.set.pages), MALLOC_CAP_SPIRAM);
    s.icon_q = heap_caps_calloc(ICONS_MAX, sizeof(*s.icon_q), MALLOC_CAP_SPIRAM);
    s.icon_get_buf = heap_caps_malloc(ICON_GET_BUF_LEN, MALLOC_CAP_SPIRAM);
    s.queue = xQueueCreate(QUEUE_LEN, sizeof(link_ev_t));
    ESP_RETURN_ON_FALSE(s.msg && s.pairing && s.session && s.set.pages && s.icon_q && s.icon_get_buf && s.queue,
                        ESP_ERR_NO_MEM, TAG, "alloc");

    esp_err_t err = pairing_load(s.pairing);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "the stored pairing is unreadable (%s), starting unpaired", esp_err_to_name(err));
    }
    set_paired(err == ESP_OK);
    if (!s.paired) {
        ESP_LOGI(TAG, "not paired");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL), TAG, "ip evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL), TAG,
                        "wifi evt");

    // Wi-Fi may already be up, in which case GOT_IP has come and gone.
    esp_netif_ip_info_t info = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
        resume();
    } else {
        s.state = ST_NO_IP;
        set_status("waiting for Wi-Fi");
    }
    if (!s.paired && s.state != ST_NO_IP) {
        set_status("not paired");
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(link_task, "link", TASK_STACK, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task");
    return ESP_OK;
}

esp_err_t link_press(const char *btn, uint32_t seq)
{
    ESP_RETURN_ON_FALSE(s.queue, ESP_ERR_INVALID_STATE, TAG, "not started");
    link_ev_t ev = {.type = EV_PRESS, .press.seq = seq};
    ESP_RETURN_ON_FALSE(strlen(btn) < sizeof(ev.press.btn), ESP_ERR_INVALID_ARG, TAG, "id too long");
    strcpy(ev.press.btn, btn);
    return post(&ev) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t link_feed_dismiss(const char *id)
{
    ESP_RETURN_ON_FALSE(s.queue, ESP_ERR_INVALID_STATE, TAG, "not started");
    link_ev_t ev = {.type = EV_FEED_DISMISS};
    ESP_RETURN_ON_FALSE(strlen(id) < sizeof(ev.press.btn), ESP_ERR_INVALID_ARG, TAG, "id too long");
    strcpy(ev.press.btn, id);
    if (!post(&ev)) {
        ESP_LOGW(TAG, "feed dismiss for %s not queued", id);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t link_pair(const link_daemon_t *daemon)
{
    ESP_RETURN_ON_FALSE(s.queue && daemon, ESP_ERR_INVALID_STATE, TAG, "not started");
    link_ev_t ev = {.type = EV_PAIR, .daemon = *daemon};
    return post(&ev) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t post_type(ev_type_t type)
{
    ESP_RETURN_ON_FALSE(s.queue, ESP_ERR_INVALID_STATE, TAG, "not started");
    link_ev_t ev = {.type = type};
    return post(&ev) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t link_pair_cancel(void)
{
    return post_type(EV_PAIR_CANCEL);
}

esp_err_t link_pair_confirm(void)
{
    return post_type(EV_PAIR_CONFIRM);
}

esp_err_t link_forget(void)
{
    return post_type(EV_FORGET);
}

const char *link_device_id(void)
{
    return s_device_id;
}
