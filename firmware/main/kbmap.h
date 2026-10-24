#pragma once

/*
 * The Wi-Fi editor's on-screen keyboard maps, as plain data for lv_keyboard_set_map. LVGL's default maps lack ~ ^ | and
 * the backtick, which a Wi-Fi password can hold. Here the three modes together hold all 95 printable ASCII characters
 * (test_kbmap.c checks that). No LVGL includes, so the host tests compile it; tab_wifi.c turns the key flags into
 * LVGL's control map.
 *
 * A text map is an LVGL button map: the key labels, "\n" between rows, "" at the end. The mode keys carry LVGL's
 * labels ("abc", "ABC", "1#"), which lv_keyboard's default handler recognizes, and the symbols are LVGL's Font Awesome
 * glyphs, which Montserrat 24 holds. tab_wifi.c checks at build time that they match LV_SYMBOL_*.
 */

#include <stddef.h>
#include <stdint.h>

#define KBMAP_BACKSPACE "\xEF\x95\x9A" // LV_SYMBOL_BACKSPACE
#define KBMAP_KEYBOARD "\xEF\x84\x9C"  // LV_SYMBOL_KEYBOARD: hides the keyboard (LV_EVENT_CANCEL)
#define KBMAP_LEFT "\xEF\x81\x93"      // LV_SYMBOL_LEFT
#define KBMAP_RIGHT "\xEF\x81\x94"     // LV_SYMBOL_RIGHT
#define KBMAP_OK "\xEF\x80\x8C"        // LV_SYMBOL_OK (LV_EVENT_READY)
#define KBMAP_MODE_LOWER "abc"         // LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_LOWER
#define KBMAP_MODE_UPPER "ABC"         // LV_KEYBOARD_CTRL_BUTTON_MODE_TEXT_UPPER
#define KBMAP_MODE_SPECIAL "1#"        // LV_KEYBOARD_CTRL_BUTTON_MODE_SPECIAL

typedef enum {
    KBMAP_LOWER,
    KBMAP_UPPER,
    KBMAP_SPECIAL,
    KBMAP_MODES,
} kbmap_mode_t;

// One byte per key, in map order (rows left to right, top to bottom): the width in the low four bits, and flags.
#define KBKEY_WIDTH_MASK 0x0F
#define KBKEY_CHECKED 0x20 // backspace, the arrows and OK, drawn in the darker key style
#define KBKEY_MODE 0x40    // mode switches and hide: no repeat, act on click

#define KBMAP_MAX_KEYS 64

/** The button map of `mode`, ending in "". */
const char *const *kbmap_text(kbmap_mode_t mode);

/** The flags and width of each key of `mode`; kbmap_key_count(mode) bytes. */
const uint8_t *kbmap_keys(kbmap_mode_t mode);

size_t kbmap_key_count(kbmap_mode_t mode);
