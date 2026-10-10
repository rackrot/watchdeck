#pragma once

#include "esp_err.h"

typedef void (*usb_status_cb_t)(const char *text);

/**
 * Expose the raw TF card (full GPT disk, no filesystem mount) to a USB host as a mass-storage
 * drive on the high-speed port. Releases the firmware's FatFs mount first (idempotent). The host
 * writes the card; this firmware never does. Progress goes to status_cb.
 * Caveat (upstream esp_tinyusb): host writes are acked before they reach the card, and a failed
 * card write is only logged, so the host never sees it. Eject before unplugging.
 */
esp_err_t usb_msc_start(usb_status_cb_t status_cb);
