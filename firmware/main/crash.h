#pragma once

#include <stdbool.h>

#include "proto.h"

/**
 * Read the coredump partition and the reset reason once, early in app_main, and keep at most one
 * pending report. A dump that fails esp_core_dump_image_check() is logged, erased and never parsed.
 */
void crash_init(void);

/** The pending report, or NULL. Valid until crash_ack() or crash_erase(). */
const proto_report_t *crash_pending(void);

/** Erase the dump and drop the pending report if `id` names it. Returns true if it did. */
bool crash_ack(const char *id);

/** Erase the dump and drop the pending report. Forget pairing calls this, since a dump may hold the token. */
void crash_erase(void);

/** With CONFIG_WATCHDECK_TEST_CRASH, dereference NULL 30 s after the first call. Otherwise do nothing. */
void crash_test_arm(void);
