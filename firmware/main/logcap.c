#include "logcap.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

/*
 * Nothing in this file may log: the hook runs inside every ESP_LOGx, so a log call here would recurse into itself.
 * Failures count in s_dropped instead.
 */

static logring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev = vprintf; // the default sink, until esp_log_set_vprintf says otherwise
static uint32_t s_dropped;

static int capture_hook(const char *fmt, va_list args)
{
    // The previous vprintf consumes `args`, so copy them first.
    va_list copy;
    va_copy(copy, args);
    const int written = s_prev(fmt, args);

    char buf[LOGRING_FMT_BUF];
    const int ret = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);

    portENTER_CRITICAL_SAFE(&s_mux);
    if (!logring_capture(&s_ring, buf, sizeof(buf), ret, ms)) {
        s_dropped++;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return written;
}

esp_err_t logcap_init(void)
{
    uint8_t *mem = heap_caps_malloc(LOGCAP_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!mem) {
        return ESP_ERR_NO_MEM;
    }
    if (!logring_init(&s_ring, mem, LOGCAP_RING_BYTES)) {
        heap_caps_free(mem);
        return ESP_ERR_INVALID_ARG;
    }
    // The ring and its lock are ready; only now can the hook run.
    s_prev = esp_log_set_vprintf(capture_hook);
    return ESP_OK;
}

void logcap_set_password(const char *pass)
{
    portENTER_CRITICAL(&s_mux);
    logring_set_password(&s_ring, pass);
    portEXIT_CRITICAL(&s_mux);
}

void logcap_set_token(const uint8_t token[PROTO_TOKEN_LEN])
{
    char hex[2 * PROTO_TOKEN_LEN + 1] = "";
    if (token) {
        for (size_t i = 0; i < PROTO_TOKEN_LEN; i++) {
            snprintf(hex + 2 * i, 3, "%02x", token[i]);
        }
    }
    portENTER_CRITICAL(&s_mux);
    logring_set_token(&s_ring, hex);
    portEXIT_CRITICAL(&s_mux);
    memset(hex, 0, sizeof(hex));
}

uint32_t logcap_dropped(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t n = s_dropped;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t logcap_next_seq(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t n = s_ring.buf ? logring_next_seq(&s_ring) : 1;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

size_t logcap_read_from(uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max)
{
    portENTER_CRITICAL(&s_mux);
    const size_t n = s_ring.buf ? logring_read_from(&s_ring, from_seq, errors_only, out, max) : 0;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

size_t logcap_read_before(uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max)
{
    portENTER_CRITICAL(&s_mux);
    const size_t n = s_ring.buf ? logring_read_before(&s_ring, before_seq, errors_only, out, max) : 0;
    portEXIT_CRITICAL(&s_mux);
    return n;
}
