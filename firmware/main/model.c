#include "model.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "icons.h"

static const char *TAG = "model";

static SemaphoreHandle_t s_lock;
static proto_layout_t *s_pages; // PROTO_MAX_PAGES slots, swapped with the link's staging slots
static uint8_t s_n_pages;
static uint32_t s_rev;
static char s_shown_id[PROTO_ID_MAX]; // the page on screen, by id, so it survives a new set and a reconnect
static proto_tile_t *s_tiles;         // MODEL_MAX_TILES slots
static size_t s_n_tiles;

esp_err_t model_init(void)
{
    ESP_RETURN_ON_FALSE(!s_lock, ESP_ERR_INVALID_STATE, TAG, "already started");
    s_pages = heap_caps_calloc(PROTO_MAX_PAGES, sizeof(*s_pages), MALLOC_CAP_SPIRAM);
    s_tiles = heap_caps_calloc(MODEL_MAX_TILES, sizeof(*s_tiles), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_pages && s_tiles && s_lock, ESP_ERR_NO_MEM, TAG, "alloc");
    return icons_init();
}

void model_lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

void model_unlock(void)
{
    xSemaphoreGive(s_lock);
}

void model_apply(pageset_t *ps)
{
    proto_layout_t *old = s_pages;
    s_pages = ps->pages;
    ps->pages = old;
    s_n_pages = ps->n_pages;
    s_rev = ps->rev;
    s_n_tiles = 0;
}

uint8_t model_pages(void)
{
    return s_n_pages;
}

uint32_t model_rev(void)
{
    return s_rev;
}

const proto_layout_t *model_page(uint8_t i)
{
    return &s_pages[i];
}

uint8_t model_shown(void)
{
    uint8_t i = 0;
    while (i < s_n_pages && strcmp(s_pages[i].page_id, s_shown_id) != 0) {
        i++;
    }
    if (i == s_n_pages) {
        i = 0;
        strlcpy(s_shown_id, s_pages[0].page_id, sizeof(s_shown_id));
    }
    return i;
}

bool model_step(int dir)
{
    model_shown(); // settles s_shown_id on a page of the set
    const int to = pageset_step(s_pages, s_n_pages, s_shown_id, dir);
    if (to < 0) {
        return false;
    }
    strlcpy(s_shown_id, s_pages[to].page_id, sizeof(s_shown_id));
    return true;
}

static bool is_tile_button(const char *btn)
{
    for (uint8_t p = 0; p < s_n_pages; p++) {
        for (size_t b = 0; b < s_pages[p].n_buttons; b++) {
            if (s_pages[p].buttons[b].tile && strcmp(s_pages[p].buttons[b].id, btn) == 0) {
                return true;
            }
        }
    }
    return false;
}

static proto_tile_t *find_tile(const char *btn)
{
    for (size_t i = 0; i < s_n_tiles; i++) {
        if (strcmp(s_tiles[i].btn, btn) == 0) {
            return &s_tiles[i];
        }
    }
    return NULL;
}

bool model_set_tile(const proto_tile_t *tile)
{
    proto_tile_t *slot = find_tile(tile->btn);
    if (!slot) {
        if (!is_tile_button(tile->btn)) {
            ESP_LOGW(TAG, "tile for %s ignored: no such tile button", tile->btn);
            return false;
        }
        if (s_n_tiles == MODEL_MAX_TILES) {
            ESP_LOGW(TAG, "tile for %s dropped: %d tile values stored already", tile->btn, MODEL_MAX_TILES);
            return false;
        }
        slot = &s_tiles[s_n_tiles++];
        ESP_LOGI(TAG, "tile for %s stored", tile->btn); // its first value since the set applied
    }
    *slot = *tile;
    return true;
}

const proto_tile_t *model_tile(const char *btn)
{
    return find_tile(btn);
}

void model_clear_tiles(void)
{
    s_n_tiles = 0;
}

void model_clear(void)
{
    s_n_pages = 0;
    s_rev = 0;
    s_shown_id[0] = '\0';
    s_n_tiles = 0;
    icons_clear();
}
