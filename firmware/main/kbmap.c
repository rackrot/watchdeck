#include "kbmap.h"

#define M KBKEY_MODE
#define C KBKEY_CHECKED

// Rows scale to the keyboard's width, so each row's widths add up to about the same total. clang-format would put one
// key to a line.
// clang-format off
static const char *const LOWER_TEXT[] = {
    KBMAP_MODE_SPECIAL, "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", KBMAP_BACKSPACE, "\n",
    KBMAP_MODE_UPPER, "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "_", "\n",
    "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "?", "\n",
    KBMAP_KEYBOARD, KBMAP_LEFT, " ", KBMAP_RIGHT, KBMAP_OK, ""};

static const uint8_t LOWER_KEYS[] = {
    M | 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, C | 7,
    M | 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    M | 2, C | 2, 6, C | 2, M | 2,
};

static const char *const UPPER_TEXT[] = {
    KBMAP_MODE_SPECIAL, "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", KBMAP_BACKSPACE, "\n",
    KBMAP_MODE_LOWER, "A", "S", "D", "F", "G", "H", "J", "K", "L", "-", "_", "\n",
    "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "?", "\n",
    KBMAP_KEYBOARD, KBMAP_LEFT, " ", KBMAP_RIGHT, KBMAP_OK, ""};

// Same shape as the lower map, so its key widths are shared.
#define UPPER_KEYS LOWER_KEYS

static const char *const SPECIAL_TEXT[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", KBMAP_BACKSPACE, "\n",
    KBMAP_MODE_LOWER, "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    "~", "^", "|", "`", "_", "-", "\n",
    KBMAP_KEYBOARD, KBMAP_LEFT, " ", KBMAP_RIGHT, KBMAP_OK, ""};

static const uint8_t SPECIAL_KEYS[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, C | 2,
    M | 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2,
    M | 2, C | 2, 6, C | 2, M | 2,
};
// clang-format on

const char *const *kbmap_text(kbmap_mode_t mode)
{
    switch (mode) {
    case KBMAP_UPPER:
        return UPPER_TEXT;
    case KBMAP_SPECIAL:
        return SPECIAL_TEXT;
    default:
        return LOWER_TEXT;
    }
}

const uint8_t *kbmap_keys(kbmap_mode_t mode)
{
    switch (mode) {
    case KBMAP_UPPER:
        return UPPER_KEYS;
    case KBMAP_SPECIAL:
        return SPECIAL_KEYS;
    default:
        return LOWER_KEYS;
    }
}

size_t kbmap_key_count(kbmap_mode_t mode)
{
    return mode == KBMAP_SPECIAL ? sizeof(SPECIAL_KEYS) : sizeof(LOWER_KEYS);
}
