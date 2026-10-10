#include "link.h"

#include <stdio.h>
#include <string.h>

#include "display.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"

#define SERVICE "_watchdeck"
#define SERVICE_PROTO "_tcp"
#define QUEUE_LEN 16
#define TASK_STACK 8192
#define DISCOVER_TIMEOUT_MS 3000
#define CONNECT_TIMEOUT_MS 15000
#define SEND_TIMEOUT_MS 2000
#define PING_INTERVAL_S 10
#define PINGPONG_TIMEOUT_S 25
#define WS_OP_TEXT 0x1

static const char *TAG = "link";

// Backoff steps in seconds; the last one is the cap. Each wait gets +/-20% jitter.
static const uint32_t BACKOFF_S[] = {1, 2, 4, 8, 16, 30};

typedef enum {
    ST_NO_IP,      // waiting for Wi-Fi
    ST_BACKOFF,    // waiting to retry (retry_at_ms)
    ST_CONNECTING, // client started, no CONNECTED yet
    ST_UP,         // hello sent
} state_t;

typedef enum {
    EV_GOT_IP,
    EV_WIFI_DOWN,
    EV_WS_UP,
    EV_WS_DOWN,
    EV_WS_MSG, // data is a PSRAM buffer the link task frees
    EV_WS_BAD, // oversize or broken frame: close the link
    EV_PRESS,
} ev_type_t;

typedef struct {
    ev_type_t type;
    uint32_t gen; // websocket events carry the client generation they came from
    union {
        struct {
            char *data;
            size_t len;
        } msg;
        struct {
            char btn[PROTO_ID_MAX];
            uint32_t seq;
        } press;
    };
} link_ev_t;

// Everything below except s_rx is touched only by the link task.
static struct {
    QueueHandle_t queue;
    link_cbs_t cbs;
    proto_msg_t *msg; // parse target, in PSRAM
    state_t state;
    esp_websocket_client_handle_t client;
    uint32_t gen;
    bool welcomed;
    size_t backoff_idx;
    int64_t retry_at_ms;
    int64_t connect_deadline_ms;
    int shown_retry_s;
} s;

// Reassembly state, touched only by the websocket task.
static char *s_rx;
static int s_rx_len;

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

static bool post(const link_ev_t *ev)
{
    if (xQueueSend(s.queue, ev, 0) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full, dropped event %d", ev->type);
        return false;
    }
    return true;
}

/* ---- Websocket event handler: runs on the client's task, so it only posts. ---- */

static void on_ws_data(const esp_websocket_event_data_t *d, uint32_t gen)
{
    // PING and PONG payloads arrive as DATA too. Fragmented text messages (continuation
    // op_code 0) are dropped on purpose, since the protocol uses single frames.
    if (d->op_code != WS_OP_TEXT) {
        if (d->op_code == 0x2) {
            ESP_LOGW(TAG, "ignoring binary frame (%d bytes)", d->payload_len);
        }
        return;
    }
    link_ev_t bad = {.type = EV_WS_BAD, .gen = gen};
    if (d->payload_len > PROTO_MAX_MSG) {
        if (d->payload_offset == 0) { // later chunks of the same frame need no second close
            ESP_LOGE(TAG, "frame of %d bytes exceeds %d, closing", d->payload_len, PROTO_MAX_MSG);
            post(&bad);
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
        post(&bad);
        return;
    }
    memcpy(s_rx + s_rx_len, d->data_ptr, d->data_len);
    s_rx_len += d->data_len;
    if (s_rx_len == d->payload_len) {
        s_rx[s_rx_len] = '\0';
        link_ev_t ev = {.type = EV_WS_MSG, .gen = gen, .msg = {.data = s_rx, .len = (size_t)s_rx_len}};
        if (!post(&ev)) {
            free(s_rx);
        }
        s_rx = NULL;
    }
}

static void ws_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    link_ev_t ev = {.gen = gen};
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ev.type = EV_WS_UP;
        post(&ev);
        break;
    case WEBSOCKET_EVENT_DATA:
        on_ws_data(data, gen);
        break;
    case WEBSOCKET_EVENT_ERROR:
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        free(s_rx);
        s_rx = NULL;
        ev.type = EV_WS_DOWN;
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

/* ---- Discovery ---- */

static bool same_subnet(uint32_t a, uint32_t b, uint32_t mask)
{
    return (a & mask) == (b & mask);
}

// Browse for the daemon. Picks an address on our own subnet if any instance has one, else the
// first IPv4 address found. Instances whose A record is missing get a follow-up query.
static bool discover(esp_ip4_addr_t *ip, uint16_t *port)
{
    esp_netif_ip_info_t ours = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_get_ip_info(sta, &ours);
    }

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(SERVICE, SERVICE_PROTO, DISCOVER_TIMEOUT_MS, 10, &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr: %s", esp_err_to_name(err));
        return false;
    }

    bool found = false;
    for (const mdns_result_t *r = results; r; r = r->next) {
        esp_ip4_addr_t cand[4];
        size_t n = 0;
        for (const mdns_ip_addr_t *a = r->addr; a && n < 4; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                cand[n++] = a->addr.u_addr.ip4;
            }
        }
        if (n == 0 && r->hostname) {
            ESP_LOGI(TAG, "%s has no address in the PTR answer, querying A", r->hostname);
            if (mdns_query_a(r->hostname, DISCOVER_TIMEOUT_MS, &cand[0]) == ESP_OK) {
                n = 1;
            }
        }
        for (size_t i = 0; i < n; i++) {
            ESP_LOGI(TAG, "candidate %s at " IPSTR ":%u", r->hostname ? r->hostname : "?", IP2STR(&cand[i]), r->port);
            bool local = same_subnet(cand[i].addr, ours.ip.addr, ours.netmask.addr);
            if (!found || (local && !same_subnet(ip->addr, ours.ip.addr, ours.netmask.addr))) {
                *ip = cand[i];
                *port = r->port;
                found = true;
            }
        }
    }
    mdns_query_results_free(results);
    return found;
}

/* ---- Session control (link task only) ---- */

static void report_retry(void)
{
    int64_t left = s.retry_at_ms - now_ms();
    int secs = left > 0 ? (int)((left + 999) / 1000) : 0;
    if (secs != s.shown_retry_s) {
        s.shown_retry_s = secs;
        set_status("offline, retry in %d s", secs);
    }
}

static void schedule_retry(void)
{
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

// Destroy the client (never from its own handler) and fence off its late events.
static void drop_client(void)
{
    s.gen++;
    if (s.client) {
        esp_websocket_client_destroy(s.client);
        s.client = NULL;
    }
    s.welcomed = false;
}

static void lost(const char *why)
{
    ESP_LOGW(TAG, "link lost: %s", why);
    drop_client();
    if (s.cbs.on_down) {
        s.cbs.on_down();
    }
    schedule_retry();
}

static void start_client(esp_ip4_addr_t ip, uint16_t port)
{
    char uri[48];
    snprintf(uri, sizeof(uri), "ws://" IPSTR ":%u/", IP2STR(&ip), port);
    s.gen++;
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .disable_auto_reconnect = true,
        .ping_interval_sec = PING_INTERVAL_S,
        .pingpong_timeout_sec = PINGPONG_TIMEOUT_S,
        .buffer_size = PROTO_MAX_MSG,
        .task_stack = 6144,
        .network_timeout_ms = 5000,
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
        lost("websocket start failed");
        return;
    }
    s.state = ST_CONNECTING;
    s.connect_deadline_ms = now_ms() + CONNECT_TIMEOUT_MS;
    set_status("connecting to " IPSTR, IP2STR(&ip));
}

static void attempt(void)
{
    set_status("searching");
    esp_ip4_addr_t ip;
    uint16_t port;
    if (!discover(&ip, &port)) {
        ESP_LOGW(TAG, "no daemon found");
        schedule_retry();
        return;
    }
    start_client(ip, port);
}

static void send_text(const char *json, size_t len)
{
    ESP_LOGI(TAG, "tx: %s", json);
    if (esp_websocket_client_send_text(s.client, json, (int)len, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) < 0) {
        lost("send failed");
    }
}

static void send_hello(void)
{
    // esp_wifi_get_mac asks the C6 for its real STA MAC. esp_read_mac(ESP_MAC_WIFI_STA) returns
    // a placeholder on the P4, which has no Wi-Fi radio of its own.
    uint8_t mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mac: %s", esp_err_to_name(err));
        lost("no MAC");
        return;
    }
    char device[PROTO_DEVICE_ID_LEN];
    proto_device_id(device, mac);
    char buf[160];
    size_t len =
        proto_build_hello(buf, sizeof(buf), device, esp_app_get_description()->version, DISPLAY_H_RES, DISPLAY_V_RES);
    if (len == 0) {
        lost("hello does not fit");
        return;
    }
    send_text(buf, len);
}

static void handle_msg(const char *json, size_t len)
{
    ESP_LOGI(TAG, "rx: %.*s", (int)(len > 160 ? 160 : len), json);
    proto_msg_t *m = s.msg;
    if (proto_parse(json, len, m) != ESP_OK) {
        ESP_LOGW(TAG, "dropped unparseable message");
        return;
    }
    switch (m->type) {
    case PROTO_MSG_WELCOME:
        s.welcomed = true;
        s.backoff_idx = 0;
        set_status("connected to %s", m->welcome_name);
        break;
    case PROTO_MSG_ERROR:
        ESP_LOGE(TAG, "daemon error %s: %s", m->error.code, m->error.msg);
        set_status("daemon error: %s", m->error.code);
        break;
    case PROTO_MSG_LAYOUT:
    case PROTO_MSG_RESULT:
        if (!s.welcomed) {
            ESP_LOGW(TAG, "message before welcome, ignored");
        } else if (m->type == PROTO_MSG_LAYOUT && s.cbs.on_layout) {
            s.cbs.on_layout(&m->layout);
        } else if (m->type == PROTO_MSG_RESULT && s.cbs.on_result) {
            s.cbs.on_result(&m->result);
        }
        break;
    case PROTO_MSG_UNKNOWN:
        ESP_LOGW(TAG, "unknown message type \"%s\", ignored", m->unknown_t);
        break;
    }
}

static void handle_event(link_ev_t *ev)
{
    bool current = ev->gen == s.gen;
    switch (ev->type) {
    case EV_GOT_IP:
        s.backoff_idx = 0;
        if (s.state == ST_NO_IP || s.state == ST_BACKOFF) {
            s.state = ST_BACKOFF;
            s.retry_at_ms = now_ms();
        }
        break;
    case EV_WIFI_DOWN:
        if (s.state != ST_NO_IP) {
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
            send_hello();
        }
        break;
    case EV_WS_DOWN:
        if (current && s.client) {
            lost("connection closed");
        }
        break;
    case EV_WS_BAD:
        if (current && s.client) {
            lost("bad frame");
        }
        break;
    case EV_WS_MSG:
        if (current && s.state == ST_UP) {
            handle_msg(ev->msg.data, ev->msg.len);
        }
        free(ev->msg.data);
        break;
    case EV_PRESS:
        if (s.state == ST_UP && s.welcomed) {
            char buf[PROTO_ID_MAX + 64];
            size_t len = proto_build_press(buf, sizeof(buf), ev->press.btn, ev->press.seq);
            if (len > 0) {
                send_text(buf, len);
            }
        } else {
            ESP_LOGW(TAG, "link down, press %u dropped", (unsigned)ev->press.seq);
        }
        break;
    }
}

static void link_task(void *arg)
{
    for (;;) {
        TickType_t wait = pdMS_TO_TICKS(1000);
        if (s.state == ST_BACKOFF) {
            int64_t left = s.retry_at_ms - now_ms();
            wait = left <= 0 ? 0 : pdMS_TO_TICKS(left < 1000 ? left : 1000) + 1;
        }
        link_ev_t ev;
        if (xQueueReceive(s.queue, &ev, wait) == pdTRUE) {
            handle_event(&ev);
        }
        if (s.state == ST_BACKOFF) {
            if (now_ms() >= s.retry_at_ms) {
                attempt();
            } else {
                report_retry();
            }
        } else if (s.state == ST_CONNECTING && now_ms() > s.connect_deadline_ms) {
            lost("connect timeout");
        }
    }
}

esp_err_t link_start(const link_cbs_t *cbs)
{
    ESP_RETURN_ON_FALSE(cbs && !s.queue, ESP_ERR_INVALID_STATE, TAG, "bad start");
    s.cbs = *cbs;
    s.msg = heap_caps_malloc(sizeof(*s.msg), MALLOC_CAP_SPIRAM);
    s.queue = xQueueCreate(QUEUE_LEN, sizeof(link_ev_t));
    ESP_RETURN_ON_FALSE(s.msg && s.queue, ESP_ERR_NO_MEM, TAG, "alloc");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL), TAG, "ip evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL), TAG,
                        "wifi evt");

    // Wi-Fi may already be up, in which case GOT_IP has come and gone.
    esp_netif_ip_info_t info = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
        s.state = ST_BACKOFF;
        s.retry_at_ms = now_ms();
    } else {
        s.state = ST_NO_IP;
        set_status("waiting for Wi-Fi");
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
