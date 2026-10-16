#include "crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "crash";

static proto_report_t report;
static bool pending;

// RISC-V mcause names, in the style of getsentry/sentry-micro (MIT), which names its faults the same
// way. Codes without an entry print as "Cause<n>".
static const char *const mcause_names[] = {
    [0] = "InstructionAddressMisaligned",
    [1] = "InstructionAccessFault",
    [2] = "IllegalInstruction",
    [3] = "Breakpoint",
    [4] = "LoadAddressMisaligned",
    [5] = "LoadAccessFault",
    [6] = "StoreAddressMisaligned",
    [7] = "StoreAccessFault",
    [8] = "EcallUMode",
    [11] = "EcallMMode",
    [12] = "InstructionPageFault",
    [13] = "LoadPageFault",
    [15] = "StorePageFault",
};

static void cause_name(uint32_t mcause, char *out, size_t len)
{
    const char *name = mcause < sizeof(mcause_names) / sizeof(mcause_names[0]) ? mcause_names[mcause] : NULL;
    if (name) {
        snprintf(out, len, "%s", name);
    } else {
        snprintf(out, len, "Cause%u", (unsigned)(mcause & 0x7fffffff));
    }
}

// The reset reason as the protocol names it, or NULL for a reset that is not abnormal.
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "int_wdt";
    case ESP_RST_TASK_WDT:
        return "task_wdt";
    case ESP_RST_WDT:
        return "wdt";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return NULL;
    }
}

// Task names come from the dump, so keep only printable ASCII.
static void copy_printable(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t n = 0;
    for (size_t i = 0; i < src_len && src[i] && n + 1 < dst_len; i++) {
        dst[n++] = src[i] >= 0x20 && src[i] <= 0x7e ? src[i] : '?';
    }
    dst[n] = '\0';
}

static void erase_dump(void)
{
    esp_err_t err = esp_core_dump_image_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump erase failed: %s", esp_err_to_name(err));
    }
}

// Fill `report` from a dump that passed the image check. Returns false if the summary is unusable.
static bool read_dump(void)
{
    esp_core_dump_summary_t *sum = calloc(1, sizeof(*sum));
    if (!sum) {
        ESP_LOGE(TAG, "no memory for the coredump summary");
        return false;
    }
    esp_err_t err = esp_core_dump_get_summary(sum);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "coredump summary: %s", esp_err_to_name(err));
        free(sum);
        return false;
    }
    report.kind = PROTO_REPORT_CRASH;
    // The summary has no reason, so take it from this boot's reset reason. A dump that outlived a
    // later power cycle or restart is still reported as a panic.
    const char *reason = reset_reason_name(esp_reset_reason());
    strlcpy(report.reason, reason && strcmp(reason, "brownout") != 0 ? reason : "panic", sizeof(report.reason));
    copy_printable(report.task, sizeof(report.task), sum->exc_task, sizeof(sum->exc_task));
    report.pc = sum->exc_pc;
    report.ra = sum->ex_info.ra;
    report.addr = sum->ex_info.mtval;
    cause_name(sum->ex_info.mcause, report.cause, sizeof(report.cause));
    copy_printable(report.elf, sizeof(report.elf), (const char *)sum->app_elf_sha256, sizeof(sum->app_elf_sha256));
    report.same_build = strncmp(report.elf, esp_app_get_elf_sha256_str(), sizeof(report.elf) - 1) == 0;
    free(sum);
    return true;
}

void crash_init(void)
{
    pending = false;
    memset(&report, 0, sizeof(report));
    snprintf(report.id, sizeof(report.id), "%08x", (unsigned)esp_random());

    esp_err_t err = esp_core_dump_image_check();
    if (err == ESP_OK) {
        pending = read_dump();
        if (!pending) {
            erase_dump(); // an unreadable summary will not get better
        }
    } else if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "coredump image is corrupt (%s), erasing it", esp_err_to_name(err));
        erase_dump();
    }

    if (!pending) {
        const char *reason = reset_reason_name(esp_reset_reason());
        if (reason && strcmp(reason, "panic") != 0) {
            report.kind = PROTO_REPORT_RESET;
            strlcpy(report.reason, reason, sizeof(report.reason));
            pending = true;
        }
    }
    if (pending) {
        ESP_LOGW(TAG, "pending %s report %s (%s), sent when the daemon connects",
                 report.kind == PROTO_REPORT_CRASH ? "crash" : "reset", report.id, report.reason);
    }
}

const proto_report_t *crash_pending(void)
{
    return pending ? &report : NULL;
}

bool crash_ack(const char *id)
{
    if (!pending || strcmp(id, report.id) != 0) {
        return false;
    }
    if (report.kind == PROTO_REPORT_CRASH) {
        erase_dump();
    }
    pending = false;
    ESP_LOGI(TAG, "report %s acked", id);
    return true;
}

void crash_erase(void)
{
    erase_dump();
    pending = false;
}

#if CONFIG_WATCHDECK_TEST_CRASH
static void test_crash_cb(void *arg)
{
    ESP_LOGW(TAG, "WATCHDECK_TEST_CRASH: dereferencing NULL");
    *(volatile int *)NULL = 1;
}
#endif

void crash_test_arm(void)
{
#if CONFIG_WATCHDECK_TEST_CRASH
    static bool armed;
    if (armed) {
        return;
    }
    armed = true;
    const esp_timer_create_args_t args = {.callback = test_crash_cb, .name = "test_crash"};
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK && esp_timer_start_once(timer, 30 * 1000000) == ESP_OK) {
        ESP_LOGW(TAG, "WATCHDECK_TEST_CRASH: crashing in 30 s");
    }
#endif
}
