#include "diag.h"

#include <string.h>

static struct {
    uint32_t count;
    char reason[DIAG_REASON_MAX];
} s_entry[DIAG_KINDS];

// One spin flag for all entries: the critical section is a short copy and the callers are rare events.
static volatile char s_lock;

static void lock(void)
{
    while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE)) {
    }
}

static void unlock(void)
{
    __atomic_clear(&s_lock, __ATOMIC_RELEASE);
}

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
    case 15:
        return "wrong password?";
    case 201:
        return "network not found";
    case 202:
        return "authentication failed";
    case 205:
        return "connection failed";
    default:
        return "other";
    }
}
