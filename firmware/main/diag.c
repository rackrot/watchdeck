#include "diag.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static struct {
    uint32_t count;
    char reason[DIAG_REASON_MAX];
} s_entry[DIAG_KINDS];

// One lock for all entries: the critical section is a short copy and the callers are rare events.
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

#define lock() portENTER_CRITICAL(&s_mux)
#define unlock() portEXIT_CRITICAL(&s_mux)

void diag_count(diag_kind_t kind, const char *reason)
{
    if ((unsigned)kind >= DIAG_KINDS) {
        return;
    }
    lock();
    s_entry[kind].count++;
    if (reason) {
        strncpy(s_entry[kind].reason, reason, DIAG_REASON_MAX - 1);
        s_entry[kind].reason[DIAG_REASON_MAX - 1] = '\0';
    }
    unlock();
}

bool diag_get(diag_kind_t kind, diag_entry_t *out)
{
    if ((unsigned)kind >= DIAG_KINDS) {
        return false;
    }
    lock();
    out->count = s_entry[kind].count;
    memcpy(out->reason, s_entry[kind].reason, DIAG_REASON_MAX);
    unlock();
    return true;
}

const char *diag_kind_name(diag_kind_t kind)
{
    switch (kind) {
    case DIAG_WIFI_DROP:
        return "Wi-Fi drops";
    case DIAG_LINK_DROP:
        return "Link drops";
    case DIAG_SET_BROKEN:
        return "Broken sets";
    case DIAG_ICON_TIMEOUT:
        return "Icon timeouts";
    case DIAG_CARD_ERROR:
        return "Card errors";
    default:
        return "";
    }
}

const char *wifi_reason_name(int code)
{
    switch (code) {
    case 2:
        return "authentication expired";
    case 3:
    case 8:
        return "access point left";
    case 15:
        return "wrong password?";
    case 200:
        return "beacon timeout";
    case 201:
        return "network not found";
    case 202:
        return "authentication failed";
    case 204:
        return "wrong password?";
    case 205:
        return "connection failed";
    default:
        return "other";
    }
}
