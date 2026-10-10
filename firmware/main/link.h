#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "proto.h"

/**
 * Callbacks into the UI. All run on the link task, so they may block briefly (they take the
 * LVGL lock) but must not call back into link_*. Any may be NULL.
 */
typedef struct {
    void (*on_status)(const char *text);             // "searching", "connected to <name>", ...
    void (*on_layout)(const proto_layout_t *layout); // valid only during the call
    void (*on_down)(void);                           // session lost, grid must stop taking presses
    void (*on_result)(const proto_result_t *result); // valid only during the call
} link_cbs_t;

/**
 * Start the link task: discover the daemon over mDNS, connect, and keep reconnecting with
 * backoff. Call after wifi_start(). `cbs` is copied.
 */
esp_err_t link_start(const link_cbs_t *cbs);

/** Queue a press for the link task to send. Safe from any task. Dropped while the link is down. */
esp_err_t link_press(const char *btn, uint32_t seq);
