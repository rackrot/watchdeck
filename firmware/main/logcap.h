#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "logring.h"
#include "proto.h"

/*
 * Captures the ESP-IDF log into a 64 KiB PSRAM ring (logring.c) for the Logs tab, through esp_log_set_vprintf. The UART
 * output is unchanged: the hook hands the original arguments to the previous vprintf first.
 *
 * Not captured: the bootloader's lines and anything before app_main (ESP-Hosted's early probe), and ESP_EARLY_LOG /
 * ESP_DRAM_LOG lines (ISR and early paths), which bypass vprintf. Log level v1 compiles debug out, so the ring holds
 * E, W and I.
 *
 * No log line may hold a token, a Wi-Fi password or a secrets.toml value. The redaction here is a backstop for the
 * token and the password only.
 */

#define LOGCAP_RING_BYTES (64 * 1024)

// The first line of app_main. Allocates the ring, then installs the hook, so the hook never sees an unready ring.
// On failure no hook is installed and the log goes to the UART alone. A second call returns ESP_ERR_INVALID_STATE.
// Nothing else may call esp_log_set_vprintf.
esp_err_t logcap_init(void);

// Logs the free stack bytes of the main tasks ("stack bytes free: ..."). One line runs about 60 s after boot; the Wi-Fi
// code asks for another after each trial. Call it from a task with room for a 160 B line, not the event loop's.
void logcap_log_stacks(void);

// Secrets the hook redacts. NULL or "" clears. A password under 8 characters is not kept (WPA's minimum; an open
// network has none), so a short one is not redacted. All are copied. The candidate password is for a trial connect:
// set it before the trial starts, and clear it when the trial ends (the saved one then goes through
// logcap_set_password).
void logcap_set_password(const char *pass);
void logcap_set_candidate_password(const char *pass);
void logcap_set_token(const uint8_t token[PROTO_TOKEN_LEN]);

// Lines the hook could not store (a formatter error). A full ring is not a drop: it overwrites the oldest.
uint32_t logcap_dropped(void);

// The sequence number the next line will get; 1 until the first line. Pass it to logcap_read_before for the newest
// page.
uint32_t logcap_next_seq(void);

// The seq of the newest E or W line stored, 0 if none. It changes only when such a line arrives.
uint32_t logcap_error_seq(void);

// The oldest sequence number still held (equal to logcap_next_seq() when the ring is empty).
uint32_t logcap_oldest_seq(void);

// Paging for the Logs tab, as logring_read_from and logring_read_before, under the ring's lock. `out` holds `max`
// lines.
size_t logcap_read_from(uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max);
size_t logcap_read_before(uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max);
