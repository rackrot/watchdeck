#include "sleep.h"

#include <string.h>

#include "display.h"
#include "ui_priv.h"

#define REFRESH_MS 1000
#define SPINNER_PX 48
#define BAR_SPIN_W 96
#define STRIP_PAD 32 // from the screen's left edge to the spinner
#define STRIP_GAP 16 // spinner to status text
#define LOGO_PX 160  // the placeholder square, until L4's art

static sleep_cfg_t s_cfg;

static lv_obj_t *s_root;
static lv_obj_t *s_els[SLEEP_EL_COUNT];
static lv_obj_t *s_strip;
static lv_obj_t *s_status;
static lv_timer_t *s_timer;
static void (*s_on_touch)(void);
static lv_obj_t *s_image_area; // the placeholder logo today; P3 puts the picture here
static lv_obj_t *s_spinner;
static bool s_spinning;
static bool s_pressed; // a touch began on the screen, so its release wakes it; one that began before it was up does not

void sleep_init(void)
{
    sleepcfg_defaults(&s_cfg);
}

const sleep_cfg_t *sleep_config(void)
{
    return &s_cfg;
}

bool sleep_is_shown(void)
{
    return s_root != NULL;
}

/* ---- Spinners, all from widgets the build already has. Each is SPINNER_PX high. ---- */

static void opa_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void bar_exec(void *obj, int32_t v)
{
    lv_bar_set_value(obj, v, LV_ANIM_OFF);
}

// One step of the pulse, 0 to 100: the circle grows and brightens, then shrinks and dims.
static void pulse_exec(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale(obj, 160 + v * 96 / 100, 0);
    lv_obj_set_style_opa(obj, (lv_opa_t)(60 + v * 195 / 100), 0);
}

static void loop_anim(lv_obj_t *obj, lv_anim_exec_xcb_t exec, int32_t from, int32_t to, uint32_t ms, uint32_t delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_reverse_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

// Not clickable, so a touch anywhere reaches the screen's own events. Animations die with their object.
static lv_obj_t *passive(lv_obj_t *obj)
{
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    return obj;
}

static lv_obj_t *circle(lv_obj_t *parent, int px, lv_color_t color)
{
    lv_obj_t *c = passive(lv_obj_create(parent));
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, px, px);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, color, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

static lv_obj_t *spinner_create(lv_obj_t *parent, sleep_spinner_t kind, lv_color_t color)
{
    switch (kind) {
    case SLEEP_SPIN_ARC: {
        lv_obj_t *sp = passive(lv_spinner_create(parent));
        lv_spinner_set_anim_params(sp, 1000, 240);
        lv_obj_set_size(sp, SPINNER_PX, SPINNER_PX);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(sp, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(sp, color, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(sp, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_arc_color(sp, color, LV_PART_MAIN);
        lv_anim_delete(sp, NULL); // spinner_run starts it
        return sp;
    }
    case SLEEP_SPIN_DOTS: {
        lv_obj_t *box = passive(lv_obj_create(parent));
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, SPINNER_PX, SPINNER_PX);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *dot = circle(box, 12, color);
            lv_obj_align(dot, LV_ALIGN_LEFT_MID, i * 18, 0);
            lv_obj_set_style_opa(dot, LV_OPA_20, 0);
        }
        return box;
    }
    case SLEEP_SPIN_BAR: {
        // Indeterminate: the value sweeps back and forth.
        lv_obj_t *bar = passive(lv_bar_create(parent));
        lv_obj_set_size(bar, BAR_SPIN_W, 10);
        lv_bar_set_range(bar, 0, 100);
        lv_obj_set_style_bg_color(bar, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, color, LV_PART_INDICATOR);
        return bar;
    }
    case SLEEP_SPIN_PULSE: {
        lv_obj_t *c = circle(parent, SPINNER_PX, color);
        lv_obj_set_style_transform_pivot_x(c, SPINNER_PX / 2, 0);
        lv_obj_set_style_transform_pivot_y(c, SPINNER_PX / 2, 0);
        return c;
    }
    default:
        return NULL; // SLEEP_SPIN_NONE
    }
}

// Start or stop the spinner's animations. They run only while the strip shows, so a link that is up costs no frames.
static void spinner_run(bool on)
{
    if (!s_spinner || s_spinning == on) {
        return;
    }
    s_spinning = on;
    switch (s_cfg.spinner) {
    case SLEEP_SPIN_ARC:
        if (on) {
            lv_spinner_set_anim_params(s_spinner, 1000, 240);
        } else {
            lv_anim_delete(s_spinner, NULL);
        }
        break;
    case SLEEP_SPIN_DOTS:
        for (uint32_t i = 0; i < lv_obj_get_child_count(s_spinner); i++) {
            lv_obj_t *dot = lv_obj_get_child(s_spinner, i);
            if (on) {
                loop_anim(dot, opa_exec, LV_OPA_20, LV_OPA_COVER, 450, i * 150);
            } else {
                lv_anim_delete(dot, NULL);
            }
        }
        break;
    case SLEEP_SPIN_BAR:
        if (on) {
            loop_anim(s_spinner, bar_exec, 0, 100, 900, 0);
        } else {
            lv_anim_delete(s_spinner, NULL);
        }
        break;
    case SLEEP_SPIN_PULSE:
        if (on) {
            loop_anim(s_spinner, pulse_exec, 0, 100, 800, 0);
        } else {
            lv_anim_delete(s_spinner, NULL);
        }
        break;
    default:
        break;
    }
}

/* ---- The screen ---- */

static void root_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_pressed = true;
    } else if (code == LV_EVENT_RELEASED && s_pressed) {
        s_pressed = false;
        s_on_touch();
    }
}

static void timer_cb(lv_timer_t *timer)
{
    (void)timer;
    sleep_refresh();
}

static const lv_font_t *el_font(sleep_el_t el)
{
    return el == SLEEP_EL_CLOCK ? &lv_font_montserrat_48 : &lv_font_montserrat_32;
}

// The widgets from the configuration. Leaves the backlight and the touch callback alone.
static void build(void)
{
    s_pressed = false;
    s_spinning = false;
    const lv_color_t fg = lv_color_hex(s_cfg.fg);

    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, DISPLAY_H_RES, DISPLAY_V_RES);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(s_cfg.bg), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_clickable(s_root, true);
    lv_obj_add_event_cb(s_root, root_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_root, root_event, LV_EVENT_RELEASED, NULL);

    // The placeholder for the logo (L4) and the picture (P3): a rounded square in the text color, centered in the area.
    lv_obj_t *logo = s_image_area = passive(lv_obj_create(s_root));
    lv_obj_remove_style_all(logo);
    lv_obj_set_size(logo, LOGO_PX, LOGO_PX);
    lv_obj_set_pos(logo, (SLEEP_AREA_W - LOGO_PX) / 2, (SLEEP_AREA_H - LOGO_PX) / 2);
    lv_obj_set_style_radius(logo, 36, 0);
    lv_obj_set_style_bg_color(logo, fg, 0);
    lv_obj_set_style_bg_opa(logo, LV_OPA_COVER, 0);

    const int align = sleepcfg_text_align(s_cfg.anchor);
    for (int i = 0; i < SLEEP_EL_COUNT; i++) {
        lv_obj_t *label = lv_label_create(s_root);
        s_els[i] = label;
        lv_obj_set_style_text_font(label, el_font(i), 0);
        lv_obj_set_style_text_color(label, fg, 0);
        lv_obj_set_style_text_align(label,
                                    align < 0   ? LV_TEXT_ALIGN_LEFT
                                    : align > 0 ? LV_TEXT_ALIGN_RIGHT
                                                : LV_TEXT_ALIGN_CENTER,
                                    0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_hidden(label, true);
    }

    s_strip = passive(lv_obj_create(s_root));
    lv_obj_remove_style_all(s_strip);
    lv_obj_set_size(s_strip, DISPLAY_H_RES, SLEEP_STRIP_H);
    lv_obj_set_pos(s_strip, 0, SLEEP_AREA_H);
    s_spinner = spinner_create(s_strip, s_cfg.spinner, lv_color_hex(s_cfg.spinner_color));
    int text_x = STRIP_PAD;
    if (s_spinner) {
        lv_obj_set_pos(s_spinner, STRIP_PAD, (SLEEP_STRIP_H - lv_obj_get_height(s_spinner)) / 2);
        text_x += lv_obj_get_width(s_spinner) + STRIP_GAP;
    }
    s_status = lv_label_create(s_strip);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_status, fg, 0);
    lv_obj_set_width(s_status, DISPLAY_H_RES - text_x - STRIP_PAD);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_status, text_x, (SLEEP_STRIP_H - lv_font_get_line_height(&lv_font_montserrat_32)) / 2);

    s_timer = lv_timer_create(timer_cb, REFRESH_MS, NULL);
    sleep_refresh();
}

static void destroy(void)
{
    lv_timer_delete(s_timer);
    s_timer = NULL;
    lv_obj_delete(s_root); // its animations go with their objects
    s_root = s_strip = s_status = s_image_area = s_spinner = NULL;
    s_spinning = false;
    memset(s_els, 0, sizeof(s_els));
}

void sleep_show(void (*on_touch)(void))
{
    if (s_root) {
        return;
    }
    s_on_touch = on_touch;
    build();
    display_backlight(s_cfg.backlight);
}

void sleep_hide(void)
{
    if (!s_root) {
        return;
    }
    destroy();
    display_backlight(100);
}

void sleep_set_config(const sleep_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (s_root) {
        destroy();
        build();
    }
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void sleep_refresh(void)
{
    if (!s_root) {
        return;
    }
    char clock[16], date[16];
    sleep_have_t have = {.time = ui_local_clock(clock, sizeof(clock), date, sizeof(date)), .host = false};
    const char *host = ui_daemon_name();
    have.host = host[0] != '\0';

    int heights[SLEEP_EL_COUNT];
    for (int i = 0; i < SLEEP_EL_COUNT; i++) {
        heights[i] = lv_font_get_line_height(el_font(i));
    }
    sleep_el_pos_t pos[SLEEP_EL_COUNT];
    sleepcfg_layout(&s_cfg, have, heights, pos);
    const char *texts[SLEEP_EL_COUNT] = {clock, date, host, s_cfg.text};
    for (int i = 0; i < SLEEP_EL_COUNT; i++) {
        lv_obj_set_hidden(s_els[i], !pos[i].shown);
        if (pos[i].shown) {
            lv_obj_set_pos(s_els[i], pos[i].x, pos[i].y);
            lv_obj_set_size(s_els[i], pos[i].w, pos[i].h);
            set_text(s_els[i], texts[i]);
        }
    }

    // The spinner and the link status show while the link is down, for either reason the screen is up.
    const bool online = ui_online();
    lv_obj_set_hidden(s_strip, online);
    spinner_run(!online);
    set_text(s_status, ui_status_text(STATUS_DAEMON));
}
