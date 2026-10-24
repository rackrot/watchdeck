#include "storagefmt.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "wifistate.h"

void storagefmt_size(char out[STORAGEFMT_SIZE_MAX], uint64_t bytes)
{
    static const char *const UNITS[] = {"KB", "MB", "GB", "TB"};
    if (bytes < 1024) {
        snprintf(out, STORAGEFMT_SIZE_MAX, "%u B", (unsigned)bytes);
        return;
    }
    size_t u = 0;
    uint64_t unit = 1024;
    while (u + 1 < sizeof(UNITS) / sizeof(UNITS[0]) && bytes >= unit * 1024) {
        unit *= 1024;
        u++;
    }
    const uint64_t tenths = bytes * 10 / unit; // rounded down, so 1023.99 KB never reads as 1024.0 KB
    snprintf(out, STORAGEFMT_SIZE_MAX, "%u.%u %s", (unsigned)(tenths / 10), (unsigned)(tenths % 10), UNITS[u]);
}

// Appends to out, which holds *len bytes already, and never writes past n.
static void append(char *out, size_t n, size_t *len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static void append(char *out, size_t n, size_t *len, const char *fmt, ...)
{
    if (*len >= n) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    const int w = vsnprintf(out + *len, n - *len, fmt, args);
    va_end(args);
    if (w > 0) {
        *len += (size_t)w;
    }
}

// Cuts `name` to printable ASCII, since a name on the card is whatever a person or a PC left there.
static void printable(char *dst, size_t dst_size, const char *name)
{
    wifiscan_printable(dst, dst_size, name ? name : "");
}

void storagefmt_card(char *out, size_t n, bool present, const char *name, uint64_t bytes, const char *fs,
                     bool read_only)
{
    if (!present) {
        snprintf(out, n, "Card: none");
        return;
    }
    char shown[16], size[STORAGEFMT_SIZE_MAX];
    printable(shown, sizeof(shown), name);
    storagefmt_size(size, bytes);
    size_t len = 0;
    append(out, n, &len, "Card: ");
    if (shown[0]) {
        append(out, n, &len, "%s, ", shown);
    }
    append(out, n, &len, "%s", size);
    if (fs) {
        append(out, n, &len, ", %s", fs);
    }
    if (read_only) {
        append(out, n, &len, ", read-only");
    }
}

void storagefmt_owner(char *out, size_t n, storagefmt_owner_t owner, bool present, bool connected, const char *error)
{
    switch (owner) {
    case STORAGEFMT_OWNER_DEVICE:
        snprintf(out, n, "Owner: on the device");
        break;
    case STORAGEFMT_OWNER_SHARED:
        snprintf(out, n, "Owner: %s", connected ? "shared over USB" : "shared, no computer attached");
        break;
    default:
        if (present) {
            snprintf(out, n, "Owner: unreadable%s%s", error && error[0] ? ": " : "", error ? error : "");
        } else {
            snprintf(out, n, "Owner: no card");
        }
        break;
    }
}

storagefmt_wifi_t storagefmt_wifi_parse(const void *data, size_t len, char ssid[WIFITXT_SSID_MAX + 1])
{
    char pass[WIFITXT_PASS_MAX + 1];
    ssid[0] = '\0';
    FILE *f = len > 0 ? fmemopen((void *)data, len, "r") : NULL; // reads in place, copying nothing
    if (!f) {
        return STORAGEFMT_WIFI_INVALID;
    }
    const wifitxt_result_t res = wifitxt_parse(f, ssid, pass);
    fclose(f);
    memset(pass, 0, sizeof(pass));
    if (res != WIFITXT_OK) {
        ssid[0] = '\0';
        return STORAGEFMT_WIFI_INVALID;
    }
    return ssid[0] ? STORAGEFMT_WIFI_FOUND : STORAGEFMT_WIFI_BLANK;
}

void storagefmt_wifi(char *out, size_t n, storagefmt_access_t access, storagefmt_wifi_t file, const char *ssid)
{
    switch (access) {
    case STORAGEFMT_READ:
        break;
    case STORAGEFMT_SHARED:
        snprintf(out, n, "wifi.txt: unknown (card shared)");
        return;
    case STORAGEFMT_NO_CARD:
        snprintf(out, n, "wifi.txt: no card");
        return;
    case STORAGEFMT_ERROR:
        snprintf(out, n, "wifi.txt: could not be read");
        return;
    default:
        snprintf(out, n, "wifi.txt: reading...");
        return;
    }
    char shown[WIFITXT_SSID_MAX + 1];
    printable(shown, sizeof(shown), ssid);
    switch (file) {
    case STORAGEFMT_WIFI_FOUND:
        snprintf(out, n, "wifi.txt: found, network %s", shown);
        break;
    case STORAGEFMT_WIFI_BLANK:
        snprintf(out, n, "wifi.txt: found, blank (connects to nothing)");
        break;
    case STORAGEFMT_WIFI_INVALID:
        snprintf(out, n, "wifi.txt: found, but not valid");
        break;
    default:
        snprintf(out, n, "wifi.txt: not found");
        break;
    }
}

void storagefmt_sleep(char *out, size_t n, storagefmt_access_t access, bool cfg, size_t images, const char *img_name,
                      uint64_t img_size)
{
    switch (access) {
    case STORAGEFMT_READ:
        break;
    case STORAGEFMT_SHARED:
        snprintf(out, n, "Sleep files: unknown (card shared)");
        return;
    case STORAGEFMT_NO_CARD:
        snprintf(out, n, "Sleep files: no card");
        return;
    case STORAGEFMT_ERROR:
        snprintf(out, n, "Sleep files: could not be read");
        return;
    default:
        snprintf(out, n, "Sleep files: reading...");
        return;
    }
    if (!cfg && images == 0) {
        snprintf(out, n, "Sleep files: none");
        return;
    }
    size_t len = 0;
    append(out, n, &len, "Sleep files: %s", cfg ? "sleep.cfg" : "no sleep.cfg");
    if (images == 0) {
        append(out, n, &len, ", no image");
        return;
    }
    char shown[STORAGEFMT_IMG_NAME], size[STORAGEFMT_SIZE_MAX];
    printable(shown, sizeof(shown), img_name);
    storagefmt_size(size, img_size);
    append(out, n, &len, ", %s %s", shown, size);
    if (images > 1) {
        append(out, n, &len, " and %u more", (unsigned)(images - 1));
    }
}

void storagefmt_free(char *out, size_t n, bool known, uint64_t free_bytes, uint64_t total)
{
    if (!known) {
        snprintf(out, n, "Free space: not checked");
        return;
    }
    char a[STORAGEFMT_SIZE_MAX], b[STORAGEFMT_SIZE_MAX];
    storagefmt_size(a, free_bytes);
    storagefmt_size(b, total);
    snprintf(out, n, "Free space: %s of %s", a, b);
}
