// The card self-test, for the board checks of the card store (plans/settings-sleep-feed/01-groundwork.md, G2). It
// writes to the real card through cardfs only: watchdeck/selftest.cfg (a small file) and write-once
// watchdeck/<8hex>.bin files, each deleted once checked, so the card holds at most one of them.
#include "cardtest.h"

#include "sdkconfig.h"

#if CONFIG_WATCHDECK_TEST_CARD

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cardfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "sdcard.h"
#include "svc.h"
#include "usb.h"

#define ROUNDS 20
#define CFG "watchdeck/selftest.cfg"
#define BIN_MAX (1024 * 1024)
#define SHARE_AFTER_US (30 * 1000 * 1000)

static const char *TAG = "cardtest";

static uint8_t *s_bin;           // BIN_MAX bytes, PSRAM
static char *s_cfg, *s_cfg_back; // CARDFS_SMALL_MAX + 1 bytes each
static uint32_t s_base;          // the counter selftest.cfg held at boot
static esp_timer_handle_t s_share_timer;

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static uint32_t ms_since(int64_t start_us)
{
    return (uint32_t)((esp_timer_get_time() - start_us) / 1000);
}

// selftest.cfg for counter: "selftest <counter>\n", then letters, 1 to 4 KiB in all, every byte from the counter.
static size_t cfg_build(uint32_t counter, char *buf)
{
    const size_t len = 1024 + mix(counter) % (CARDFS_SMALL_MAX - 1024 + 1);
    const int head = snprintf(buf, len, "selftest %lu\n", (unsigned long)counter);
    for (size_t i = (size_t)head; i < len; i++) {
        buf[i] = (char)('a' + (counter + i) % 26);
    }
    return len;
}

// The image for counter: 0 to 1 MiB of bytes from a counter-seeded xorshift.
static size_t bin_build(uint32_t counter, uint8_t *buf)
{
    const size_t len = mix(counter ^ 0x9e3779b9u) % (BIN_MAX + 1);
    uint32_t x = counter | 1;
    for (size_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
    return len;
}

static void share_cb(void *arg)
{
    const esp_err_t err = usb_share();
    ESP_LOGI(TAG, "card self-test: sharing the card over USB: %s", esp_err_to_name(err));
}

static void finish(unsigned done)
{
    ESP_LOGI(TAG, "card self-test: %u/%u rounds ok; sharing the card over USB in 30 s", done, ROUNDS);
    esp_timer_start_once(s_share_timer, SHARE_AFTER_US);
}

static void fail(unsigned round, const char *what, esp_err_t err)
{
    ESP_LOGE(TAG, "card self-test %u/%u FAILED: %s: %s", round, ROUNDS, what, esp_err_to_name(err));
    sdcard_note_failure(what, err); // counts it, and remounts after a card access failed
    finish(round - 1);
}

// The name of a self-test image, <8hex>.bin, into digest (the first 4 bytes of its SHA-256).
static bool bin_name(const char *name, uint8_t digest[4])
{
    if (strlen(name) != 12 || strspn(name, "0123456789abcdef") != 8 || strcmp(name + 8, ".bin") != 0) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        unsigned v;
        sscanf(name + 2 * i, "%2x", &v);
        digest[i] = (uint8_t)v;
    }
    return true;
}

// Logs each entry of watchdeck/, and checks then deletes a self-test image a cut left behind.
static void on_entry(const char *name, size_t size, bool is_dir, void *ctx)
{
    ESP_LOGI(TAG, "  watchdeck/%s%s (%u bytes)", name, is_dir ? "/" : "", (unsigned)size);
    uint8_t digest[4];
    if (is_dir || !bin_name(name, digest)) {
        return;
    }
    char rel[32];
    snprintf(rel, sizeof(rel), "watchdeck/%s", name);
    const esp_err_t verdict = cardfs_verify(rel, size, digest, sizeof(digest));
    const esp_err_t removed = cardfs_remove(rel);
    ESP_LOGW(TAG, "  left over: %s is %s; deleted: %s", rel,
             verdict == ESP_OK                ? "valid"
             : verdict == ESP_ERR_INVALID_CRC ? "invalid"
                                              : esp_err_to_name(verdict),
             esp_err_to_name(removed));
}

// Reads selftest.cfg and checks every byte against its counter. Sets s_base.
static esp_err_t check_cfg(void)
{
    size_t len;
    esp_err_t err = cardfs_read(CFG, s_cfg_back, CARDFS_SMALL_MAX, &len);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "%s: absent, counting from 0", CFG);
        s_base = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    s_cfg_back[len] = '\0';
    unsigned long counter;
    if (sscanf(s_cfg_back, "selftest %lu\n", &counter) != 1) {
        ESP_LOGE(TAG, "%s: no counter in its %u bytes", CFG, (unsigned)len);
        return ESP_ERR_INVALID_CRC;
    }
    const size_t want = cfg_build((uint32_t)counter, s_cfg);
    if (want != len || memcmp(s_cfg, s_cfg_back, len) != 0) {
        ESP_LOGE(TAG, "%s: counter %lu, but %u bytes that differ from what it wrote", CFG, counter, (unsigned)len);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "%s: counter %lu, intact (%u bytes)", CFG, counter, (unsigned)len);
    s_base = (uint32_t)counter;
    return ESP_OK;
}

static void round_job(void *arg)
{
    const unsigned round = (unsigned)(uintptr_t)arg;
    const uint32_t counter = s_base + round;

    const size_t cfg_len = cfg_build(counter, s_cfg);
    int64_t start = esp_timer_get_time();
    esp_err_t err = cardfs_write_small(CFG, s_cfg, cfg_len);
    const uint32_t cfg_ms = ms_since(start);
    if (round == 1) {
        ESP_LOGI(TAG, "first write since the mount (its first cluster allocation): %lu ms", (unsigned long)cfg_ms);
    }
    if (err != ESP_OK) {
        fail(round, "write " CFG, err);
        return;
    }
    size_t len;
    err = cardfs_read(CFG, s_cfg_back, CARDFS_SMALL_MAX, &len);
    if (err == ESP_OK && (len != cfg_len || memcmp(s_cfg, s_cfg_back, len) != 0)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        fail(round, "read back " CFG, err);
        return;
    }

    const size_t bin_len = bin_build(counter, s_bin);
    uint8_t hash[32];
    size_t hash_len;
    if (psa_hash_compute(PSA_ALG_SHA_256, s_bin, bin_len, hash, sizeof(hash), &hash_len) != PSA_SUCCESS) {
        fail(round, "hash the image", ESP_FAIL);
        return;
    }
    char rel[32];
    snprintf(rel, sizeof(rel), "watchdeck/%02x%02x%02x%02x.bin", hash[0], hash[1], hash[2], hash[3]);
    start = esp_timer_get_time();
    err = cardfs_write_new(rel, s_bin, bin_len);
    const uint32_t bin_ms = ms_since(start);
    if (err != ESP_OK) {
        fail(round, "write the image", err);
        return;
    }
    start = esp_timer_get_time();
    err = cardfs_verify(rel, bin_len, hash, sizeof(hash)); // reads every byte back
    const uint32_t verify_ms = ms_since(start);
    if (err != ESP_OK) {
        fail(round, "read back the image", err);
        return;
    }
    err = cardfs_remove(rel);
    if (err != ESP_OK) {
        fail(round, "delete the image", err);
        return;
    }
    ESP_LOGI(TAG, "card self-test %u/%u ok (%s %u bytes in %lu ms; %s %u bytes in %lu ms, read back in %lu ms)", round,
             ROUNDS, CFG, (unsigned)cfg_len, (unsigned long)cfg_ms, rel, (unsigned)bin_len, (unsigned long)bin_ms,
             (unsigned long)verify_ms);
    if (bin_ms > 3000) {
        ESP_LOGW(TAG, "a 1 MiB write over 3 s is a finding (01-groundwork.md, risks)");
    }

    if (round == ROUNDS) {
        finish(ROUNDS);
    } else if ((err = svc_post(round_job, (void *)(uintptr_t)(round + 1))) != ESP_OK) {
        ESP_LOGE(TAG, "card self-test: can't queue round %u: %s", round + 1, esp_err_to_name(err));
        finish(round);
    }
}

static void start_job(void *arg)
{
    if (sdcard_owner() != SDCARD_DEVICE || !cardfs_writable()) {
        ESP_LOGW(TAG, "card self-test skipped: the card isn't writable");
        return;
    }
    s_bin = heap_caps_malloc(BIN_MAX, MALLOC_CAP_SPIRAM);
    s_cfg = malloc(CARDFS_SMALL_MAX + 1);
    s_cfg_back = malloc(CARDFS_SMALL_MAX + 1);
    if (!s_bin || !s_cfg || !s_cfg_back) {
        ESP_LOGE(TAG, "card self-test: no memory");
        return;
    }
    ESP_LOGW(TAG, "card self-test: %u rounds of writes to the TF card", ROUNDS);
    ESP_LOGI(TAG, "watchdeck/ holds:");
    esp_err_t err = cardfs_list(on_entry, NULL);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "  (listing: %s)", esp_err_to_name(err));
    }
    err = check_cfg();
    if (err != ESP_OK) {
        fail(1, "check " CFG, err);
        return;
    }
    if ((err = svc_post(round_job, (void *)(uintptr_t)1)) != ESP_OK) {
        ESP_LOGE(TAG, "card self-test: can't queue round 1: %s", esp_err_to_name(err));
    }
}

void cardtest_start(void)
{
    const esp_timer_create_args_t args = {.callback = share_cb, .name = "cardtest_share"};
    esp_err_t err = esp_timer_create(&args, &s_share_timer);
    if (err == ESP_OK) {
        err = svc_post(start_job, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "card self-test not started: %s", esp_err_to_name(err));
    }
}

#else

void cardtest_start(void)
{
}

#endif
