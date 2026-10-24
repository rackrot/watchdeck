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
// On failure no hook is installed and the log goes to the UART alone.
esp_err_t logcap_init(void);

// Secrets the hook redacts. NULL or "" clears. A password under 4 characters is not kept. Both are copied.
void logcap_set_password(const char *pass);
void logcap_set_token(const uint8_t token[PROTO_TOKEN_LEN]);

// Lines the hook could not store (a formatter error). A full ring is not a drop: it overwrites the oldest.
uint32_t logcap_dropped(void);

// The sequence number the next line will get; 1 until the first line. Pass it to logcap_read_before for the newest
// page.
uint32_t logcap_next_seq(void);

// Paging for the Logs tab, as logring_read_from and logring_read_before, under the ring's lock. `out` holds `max`
// lines.
size_t logcap_read_from(uint32_t from_seq, bool errors_only, logring_line_t *out, size_t max);
size_t logcap_read_before(uint32_t before_seq, bool errors_only, logring_line_t *out, size_t max);
