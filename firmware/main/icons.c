#include "icons.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "misc/cache/instance/lv_image_cache.h"

static const char *TAG = "icons";

static struct {
    bool used;
    uint8_t id[PROTO_ICON_ID_LEN];
    uint8_t *frame; // the PSRAM block the pixels live in
    size_t bytes;
    lv_image_dsc_t dsc;
} s_icons[ICONS_MAX];
static size_t s_bytes;

static int find(const uint8_t id[PROTO_ICON_ID_LEN])
{
    for (int i = 0; i < ICONS_MAX; i++) {
        if (s_icons[i].used && memcmp(s_icons[i].id, id, PROTO_ICON_ID_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

static void drop(int i)
{
    // LVGL's image cache is off (LV_CACHE_DEF_SIZE=0), so this does nothing today. It keeps a
    // later change that turns the cache on from serving a freed or reused pointer.
    lvgl_port_lock(0);
    lv_image_cache_drop(&s_icons[i].dsc);
    lvgl_port_unlock();
    s_bytes -= s_icons[i].bytes;
    free(s_icons[i].frame);
    memset(&s_icons[i], 0, sizeof(s_icons[i]));
}

const lv_image_dsc_t *icons_find(const uint8_t id[PROTO_ICON_ID_LEN])
{
    int i = find(id);
    return i < 0 ? NULL : &s_icons[i].dsc;
}

esp_err_t icons_add(const proto_icon_t *icon, uint8_t *frame)
{
    if (find(icon->id) >= 0) {
        ESP_LOGW(TAG, "icon already cached");
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = (size_t)icon->w * icon->h * 4;
    if (s_bytes + bytes > ICONS_MAX_BYTES) {
        ESP_LOGW(TAG, "icon of %u bytes would pass the %d byte cache cap", (unsigned)bytes, ICONS_MAX_BYTES);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < ICONS_MAX; i++) {
        if (!s_icons[i].used) {
            s_icons[i].used = true;
            memcpy(s_icons[i].id, icon->id, PROTO_ICON_ID_LEN);
            s_icons[i].frame = frame;
            s_icons[i].bytes = bytes;
            s_icons[i].dsc = (lv_image_dsc_t){
                .header = {.magic = LV_IMAGE_HEADER_MAGIC,
                           .cf = LV_COLOR_FORMAT_ARGB8888,
                           .w = icon->w,
                           .h = icon->h,
                           .stride = icon->w * 4},
                .data_size = (uint32_t)bytes,
                .data = icon->pixels,
            };
            s_bytes += bytes;
            return ESP_OK;
        }
    }
    ESP_LOGW(TAG, "icon cache full");
    return ESP_ERR_NO_MEM;
}

size_t icons_retain(const proto_layout_t *layout)
{
    size_t dropped = 0;
    for (int i = 0; i < ICONS_MAX; i++) {
        if (!s_icons[i].used) {
            continue;
        }
        bool named = false;
        for (size_t b = 0; b < layout->n_buttons && !named; b++) {
            named =
                layout->buttons[b].has_icon && memcmp(layout->buttons[b].icon, s_icons[i].id, PROTO_ICON_ID_LEN) == 0;
        }
        if (!named) {
            drop(i);
            dropped++;
        }
    }
    return dropped;
}

void icons_clear(void)
{
    for (int i = 0; i < ICONS_MAX; i++) {
        if (s_icons[i].used) {
            drop(i);
        }
    }
}
