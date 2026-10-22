#include "font_ids.h"

#include <string.h>

static const char *const NAMES[WD_FONT_COUNT] = {
#define X(id, name) [WD_FONT_##id] = name,
    WD_FONT_LIST(X)
#undef X
};

wd_font_t wd_font_from_name(const char *name, bool *known)
{
    for (int i = 0; name && i < WD_FONT_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) {
            if (known) {
                *known = true;
            }
            return (wd_font_t)i;
        }
    }
    if (known) {
        *known = false;
    }
    return WD_FONT_MONTSERRAT;
}
