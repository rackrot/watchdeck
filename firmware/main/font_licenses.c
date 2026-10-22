#include "font_licenses.h"

extern const char font_licenses_start[] asm("_binary_font_licenses_txt_start");

const char *font_licenses_text(void)
{
    return font_licenses_start;
}
