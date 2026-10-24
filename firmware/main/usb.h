#pragma once

#include "esp_err.h"

typedef void (*usb_status_cb_t)(const char *text);

/**
 * At boot: install TinyUSB on the high-speed port (J16) and its mass-storage class with esp_tinyusb's own mounting
 * off, mapping no storage, so a host sees an empty card reader. The card stays the firmware's until usb_share.
 * The USB status line goes to status_cb: "USB: card on the device", "USB: sharing the card (connected to a
 * computer)", "USB: sharing the card (no computer)", "USB: no card" or "USB: error (<err>)".
 */
esp_err_t usb_start(usb_status_cb_t status_cb);

/**
 * Queue the share on svc: it waits for any card write to finish, turns cardfs writes off, unmounts the card and maps
 * the whole raw card (GPT disk) to USB, never formatting it. If mapping fails the card is mounted again.
 * ESP_ERR_INVALID_STATE: USB isn't up, the card isn't the device's, or a share is already queued. ESP_ERR_NO_MEM:
 * svc is busy. Caveat (upstream esp_tinyusb): host writes are acked before they reach the card, and a failed card
 * write is only logged, so the host never sees it. The host ejects before unplugging.
 */
esp_err_t usb_share(void);

/**
 * Stop sharing: restart the board, which ends the USB session and mounts the card again at boot (about 8 s; the
 * pairing, NVS and the coredump stay). Taking the card back without a restart races esp_tinyusb's SCSI callbacks.
 * Ask the user to eject the drive on the computer first.
 */
void usb_stop_sharing(void);

/** Shows the USB status line again; called when the card's owner changes. */
void usb_refresh_status(void);
