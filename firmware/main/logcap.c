#include "logcap.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Define LOGCAP_TIMING to log the cycle count of every ring read (after the lock is released), to find the worst case
 * on the board. The CPU runs at 360 MHz or 400 MHz, so 1000 cycles are under 3 us.
 */
// #define LOGCAP_TIMING
#ifdef LOGCAP_TIMING
#include "esp_cpu.h"
#define TIMING_START() const uint32_t t0_ = esp_cpu_get_cycle_count()
#define TIMING_END(what, n)                                                                                            \
    ESP_LOGI(TAG, "ring read %s: %u cycles, %u lines", what, (unsigned)(esp_cpu_get_cycle_count() - t0_), (unsigned)(n))
#else
#define TIMING_START()
#define TIMING_END(what, n)
#endif

#define STACK_LOG_DELAY_US (60 * 1000 * 1000)

static const char *TAG = "logcap";

/*
 * Nothing in the hook may log: it runs inside every ESP_LOGx, so a log call there would recurse into itself. Failures
 * count in s_dropped instead.
 */

static logring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev = vprintf; // the default sink, until esp_log_set_vprintf says otherwise
static uint32_t s_dropped;
static bool s_started;

static int capture_hook(const char *fmt, va_list args)
{
    // Before s_prev, which may block on the UART: the time is when the line was logged.
    const uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);

    // The previous vprintf consumes `args`, so copy them first.
    va_list copy;
    va_copy(copy, args);
    const int written = s_prev(fmt, args);

    char buf[LOGRING_FMT_BUF];
    const int ret = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);

    portENTER_CRITICAL_SAFE(&s_mux);
    if (!logring_capture(&s_ring, buf, sizeof(buf), ret, ms)) {
        s_dropped++;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
    return written;
}

// The tasks whose stacks the plan checks. Names that don't exist (svc before it lands, hosted's names) read "absent".
static const char *const s_stack_tasks[] = {"link",      "LVGL task", "svc", "sys_evt",   "ipc0",       "ipc1",
                                            "esp_timer", "wifi",      "tiT", "sdio_read", "sdio_write", "process_rx"};

static void log_stacks(void *arg)
{
    char line[160];
    size_t at = snprintf(line, sizeof(line), "stack bytes free:");
    for (size_t i = 0; i < sizeof(s_stack_tasks) / sizeof(s_stack_tasks[0]) && at < sizeof(line); i++) {
        const TaskHandle_t t = xTaskGetHandle(s_stack_tasks[i]);
        if (t) {
            at += snprintf(line + at, sizeof(line) - at, " %s=%u", s_stack_tasks[i],
                           (unsigned)uxTaskGetStackHighWaterMark(t));
        } else {
            at += snprintf(line + at, sizeof(line) - at, " %s=absent", s_stack_tasks[i]);
        }
    }
    ESP_LOGI(TAG, "%s", line);
}

esp_err_t logcap_init(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t *mem = heap_caps_malloc(LOGCAP_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (!mem) {
        return ESP_ERR_NO_MEM;
    }
    if (!logring_init(&s_ring, mem, LOGCAP_RING_BYTES)) {
        heap_caps_free(mem);
        return ESP_ERR_INVALID_ARG;
    }
    // The ring and its lock are ready; only now can the hook run.
    s_started = true;
    s_prev = esp_log_set_vprintf(capture_hook);

    // One line about 60 s after boot, for the stack check. A failure only loses that line.
    static esp_timer_handle_t stack_timer;
    const esp_timer_create_args_t args = {.callback = log_stacks, .name = "stack_log"};
    if (esp_timer_create(&args, &stack_timer) == ESP_OK) {
        esp_timer_start_once(stack_timer, STACK_LOG_DELAY_US);
    }
    return ESP_OK;
}

static void set_password(int slot, const char *pass)
{
    portENTER_CRITICAL(&s_mux);
    logring_set_password(&s_ring, slot, pass);
    portEXIT_CRITICAL(&s_mux);
}

void logcap_set_password(const char *pass)
{
    set_password(0, pass);
}

void logcap_set_candidate_password(const char *pass)
{
    set_password(1, pass);
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

uint32_t logcap_oldest_seq(void)
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t n = s_ring.buf ? logring_oldest_seq(&s_ring) : 1;
    portEXIT_CRITICAL(&s_mux);
    return n;
}

size_t logcap_read_from(uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max)
{
    TIMING_START();
    portENTER_CRITICAL(&s_mux);
    const size_t n = s_ring.buf ? logring_read_from(&s_ring, from_seq, errors_only, out, max) : 0;
    portEXIT_CRITICAL(&s_mux);
    TIMING_END(errors_only ? "from (errors)" : "from", n);
    return n;
}

size_t logcap_read_before(uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max)
{
    TIMING_START();
    portENTER_CRITICAL(&s_mux);
    const size_t n = s_ring.buf ? logring_read_before(&s_ring, before_seq, errors_only, out, max) : 0;
    portEXIT_CRITICAL(&s_mux);
    TIMING_END(errors_only ? "before (errors)" : "before", n);
    return n;
}
