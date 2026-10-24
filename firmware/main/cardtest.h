#pragma once

/**
 * With CONFIG_WATCHDECK_TEST_CARD, queue the card self-test on svc: list watchdeck/, check and delete leftover
 * <8hex>.bin files, check watchdeck/selftest.cfg, then 20 rounds of a small write and a write-once image, each read
 * back, then share the card over USB 30 s later. Otherwise do nothing. Call after sdcard_mount, svc_start and
 * usb_start.
 */
void cardtest_start(void);
