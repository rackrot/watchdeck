#pragma once

// wifi.txt on the TF card: line 1 the SSID, line 2 the password, LF or CRLF. No line 2 is an open network, and a
// blank line 1 means "don't connect". site/src/wifi.md documents the format.

#include <stddef.h>
#include <stdio.h>

#define WIFITXT_SSID_MAX 32
#define WIFITXT_PASS_MAX 64
#define WIFITXT_FILE_MAX (WIFITXT_SSID_MAX + 1 + WIFITXT_PASS_MAX + 1) // "SSID\npassword\n"

typedef enum {
    WIFITXT_OK,        // ssid and pass are set; ssid "" means don't connect, pass "" an open network
    WIFITXT_EMPTY,     // the file has no first line
    WIFITXT_SSID_LONG, // line 1 is over WIFITXT_SSID_MAX bytes
    WIFITXT_PASS_LONG, // line 2 is over WIFITXT_PASS_MAX bytes
} wifitxt_result_t;

// Reads the credentials. Nothing else about them is checked, as wifi.c always did.
wifitxt_result_t wifitxt_parse(FILE *f, char ssid[WIFITXT_SSID_MAX + 1], char pass[WIFITXT_PASS_MAX + 1]);

// Writes the file's bytes: "SSID\n" for an open network (pass ""), else "SSID\npassword\n". Returns the length,
// or 0 when refused: an SSID of 0 or over 32 bytes or with CR or LF, or a password that isn't empty, 8 to 63
// printable ASCII characters, or 64 hex digits.
size_t wifitxt_format(const char *ssid, const char *pass, char buf[WIFITXT_FILE_MAX]);
