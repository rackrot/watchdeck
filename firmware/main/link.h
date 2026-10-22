#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"
#include "lvgl.h"
#include "proto.h"

#define LINK_MAX_DAEMONS 4 // daemons open for pairing that the unpaired screen lists

/** A daemon open for pairing, as mDNS shows it. */
typedef struct {
    char name[PROTO_NAME_MAX]; // mDNS instance name, maybe truncated
    esp_ip4_addr_t ip;
    uint16_t port; // its pairing port, from TXT "pair"
} link_daemon_t;

/**
 * Callbacks into the UI. All run on the link task, never with the model lock held, so they may
 * block briefly (they take the LVGL lock, then maybe the model lock) but must not call back into
 * link_*, except link_watch_rssi, which only sets a flag. Any may be NULL.
 */
typedef struct {
    void (*on_status)(const char *text);  // "searching", "connected to <name>", ...
    void (*on_welcome)(const char *name); // the daemon's host name, at each welcome; valid during the call
    void (*on_set)(void);                 // a set of pages is in the model: rebuild the page on screen from it
    void (*on_icon)(const uint8_t id[PROTO_ICON_ID_LEN],
                    const lv_image_dsc_t *dsc);                    // show it on every button with `id`
    void (*on_icons_settled)(void);                                // every icon_get of the set is answered or timed out
    void (*on_down)(void);                                         // session lost, grid must stop taking presses
    void (*on_result)(const proto_result_t *result);               // valid only during the call
    void (*on_tile)(const proto_tile_t *tile);                     // stored in the model already; valid during the call
    void (*on_paired)(bool paired);                                // at start and on every change
    void (*on_daemons)(const link_daemon_t *daemons, size_t n);    // unpaired: the pairable list changed
    void (*on_pair_code)(const char *code, const char *name);      // show the code, or NULL when the attempt ends
    void (*on_pair_confirm)(const char *device, const char *name); // ask: does `name` show "Paired <device>"?
} link_cbs_t;

/**
 * Start the link task. Paired, it finds the daemon over mDNS, connects, and keeps reconnecting
 * with backoff. Unpaired, it lists the daemons open for pairing. Call after wifi_start().
 * `cbs` is copied.
 */
esp_err_t link_start(const link_cbs_t *cbs);

/** Queue a press for the link task to send. Safe from any task. Dropped while the link is down. */
esp_err_t link_press(const char *btn, uint32_t seq);

/** Pair with `daemon`. Ignored unless unpaired and idle. Safe from any task. */
esp_err_t link_pair(const link_daemon_t *daemon);

/** Abandon the pairing attempt in progress, or answer No to its question. Safe from any task. */
esp_err_t link_pair_cancel(void);

/** Answer Yes to the attempt's question: save the pairing and connect. Safe from any task. */
esp_err_t link_pair_confirm(void);

/** Forget the pairing and go back to the unpaired list. Safe from any task. */
esp_err_t link_forget(void);

/**
 * This device's id ("wd-" and the C6's MAC), or "" before the link first read it. The link reads it before its first
 * hello, so it is set by the time a set applies. Safe from any task.
 */
const char *link_device_id(void);

/**
 * While `on`, the link task refreshes the cached RSSI (wifi_rssi) at once and then every few seconds, so a reader on
 * the LVGL task never makes the RPC itself. Safe from any task.
 */
void link_watch_rssi(bool on);
