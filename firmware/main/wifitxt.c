#include "wifitxt.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

typedef enum { LINE_OK, LINE_EOF, LINE_LONG } line_t;

// Reads one line into out (max_len + 1 bytes) without its line ending.
static line_t read_line(FILE *f, char *out, size_t max_len)
{
    char buf[WIFITXT_PASS_MAX + 3]; // the longest line plus CR, LF and NUL
    if (!fgets(buf, (int)(max_len + 3), f)) {
        return LINE_EOF;
    }
    const bool has_newline = strchr(buf, '\n') != NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    const size_t len = strlen(buf);
    if ((!has_newline && !feof(f)) || len > max_len) {
        return LINE_LONG;
    }
    memcpy(out, buf, len + 1);
    return LINE_OK;
}

wifitxt_result_t wifitxt_parse(FILE *f, char ssid[WIFITXT_SSID_MAX + 1], char pass[WIFITXT_PASS_MAX + 1])
{
    ssid[0] = '\0';
    pass[0] = '\0';
    switch (read_line(f, ssid, WIFITXT_SSID_MAX)) {
    case LINE_EOF:
        return WIFITXT_EMPTY;
    case LINE_LONG:
        return WIFITXT_SSID_LONG;
    case LINE_OK:
        break;
    }
    const int c = fgetc(f);
    if (c == EOF) {
        return WIFITXT_OK; // no password line: an open network
    }
    ungetc(c, f);
    return read_line(f, pass, WIFITXT_PASS_MAX) == LINE_OK ? WIFITXT_OK : WIFITXT_PASS_LONG;
}

static bool pass_ok(const char *pass, size_t len)
{
    if (len == 0) {
        return true;
    }
    if (len == 64) {
        for (size_t i = 0; i < len; i++) {
            if (!isxdigit((unsigned char)pass[i])) {
                return false;
            }
        }
        return true;
    }
    if (len < 8 || len > 63) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pass[i] < 0x20 || pass[i] > 0x7e) {
            return false;
        }
    }
    return true;
}

size_t wifitxt_format(const char *ssid, const char *pass, char buf[WIFITXT_FILE_MAX])
{
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(pass);
    if (ssid_len == 0 || ssid_len > WIFITXT_SSID_MAX || strpbrk(ssid, "\r\n") || !pass_ok(pass, pass_len)) {
        return 0;
    }
    size_t n = 0;
    memcpy(buf, ssid, ssid_len);
    n += ssid_len;
    buf[n++] = '\n';
    if (pass_len > 0) {
        memcpy(buf + n, pass, pass_len);
        n += pass_len;
        buf[n++] = '\n';
    }
    return n;
}
