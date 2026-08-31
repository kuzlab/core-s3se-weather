#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lw_fonts.h"
#include "sdkconfig.h"

static const char *TAG = "ui";

#define SCREEN_W 320
#define SCREEN_H 240

/* Layout, top to bottom. Constants rather than a layout engine because the
 * screen is a fixed size and every element has one place it belongs. */
#define PAD_X            8
#define HEADER_Y         4
#define BANNER_Y         26
#define BANNER_H         60
#define SUMMARY1_Y       92
#define SUMMARY2_Y       112
#define CAPTION_Y        134
#define GRAPH_Y          154
#define GRAPH_H          46
#define AXIS_Y           202

#define GRAPH_X          PAD_X
#define GRAPH_W          (SCREEN_W - 2 * PAD_X)

#define ICON_BOX         52
#define ICON_X           8
#define BANNER_TEXT_X    (ICON_X + ICON_BOX + 8)

/* --- palettes -----------------------------------------------------------
 * Two schemes, swapped on the clock. The day scheme is light because the
 * thing sits in a living space; at night the same brightness is glare, and
 * turning the backlight down alone cannot fix that -- DLDO1 only spans
 * 2.5-3.3V, so most of the reduction has to come from what is drawn.
 *
 * Verdict colours are a tint behind same-hue text in both schemes, which
 * reads as friendlier than white-on-saturated and holds more contrast. */
typedef struct {
    uint32_t bg, card, text, dim, border;
    uint32_t ok_bg, ok_fg;
    uint32_t caution_bg, caution_fg;
    uint32_t bring_in_bg, bring_in_fg;
    uint32_t raining_bg, raining_fg;
    uint32_t unknown_bg, unknown_fg;
    uint32_t sun, sun_halo, cloud, cloud_light, drop;
    uint32_t pop_bar, mm_bar, mm_rainy;
} ui_palette_t;

static const ui_palette_t PALETTE_DAY = {
    .bg = 0xF6F3EC, .card = 0xFFFFFF, .text = 0x3D4450,
    .dim = 0x929AA5, .border = 0xE6E1D8,
    .ok_bg       = 0xE7F6EA, .ok_fg       = 0x2E7D4F,
    .caution_bg  = 0xFFF4DC, .caution_fg  = 0xA9760A,
    .bring_in_bg = 0xFFE9DC, .bring_in_fg = 0xC0551D,
    .raining_bg  = 0xFFE4E6, .raining_fg  = 0xC0392B,
    .unknown_bg  = 0xEDEBE6, .unknown_fg  = 0x8A9199,
    .sun = 0xFFC93C, .sun_halo = 0xFFE7A3,
    .cloud = 0xAFC0D2, .cloud_light = 0xC9D6E3, .drop = 0x4A90D9,
    .pop_bar = 0xDCE7F3, .mm_bar = 0x86B8E6, .mm_rainy = 0x3C7FC4,
};

/* Deliberately not pure black: a dark grey lets the tinted banner read as a
 * colour rather than as the only lit thing on the panel. Foregrounds are
 * held well below white so nothing glares in a dark room. */
static const ui_palette_t PALETTE_NIGHT = {
    .bg = 0x14171C, .card = 0x1B1F26, .text = 0xB6BEC8,
    .dim = 0x69717C, .border = 0x282D36,
    .ok_bg       = 0x16301F, .ok_fg       = 0x63B47F,
    .caution_bg  = 0x322708, .caution_fg  = 0xC79A3C,
    .bring_in_bg = 0x38200F, .bring_in_fg = 0xD1794A,
    .raining_bg  = 0x361419, .raining_fg  = 0xCF6B78,
    .unknown_bg  = 0x1F232A, .unknown_fg  = 0x69717C,
    .sun = 0xB08A28, .sun_halo = 0x3B3117,
    .cloud = 0x4E5967, .cloud_light = 0x616D7C, .drop = 0x3C6E9E,
    .pop_bar = 0x1E2A3A, .mm_bar = 0x3C6E9E, .mm_rainy = 0x5B9BD5,
};

static const ui_palette_t *s_pal = &PALETTE_DAY;

/* Bars are pre-created for the widest range so switching 24h <-> 12h only
 * resizes them; creating and deleting objects on every update would churn
 * LVGL's heap for no reason. */
#define MAX_BARS         24
#define AXIS_LABEL_STEP  3
#define MAX_AXIS_LABELS  (MAX_BARS / AXIS_LABEL_STEP + 1)

#define N_DROPS          3
#define DROP_FALL_MS     900

/* Never wait forever for the LVGL lock. esp_lvgl_port treats 0 as
 * portMAX_DELAY, and the LVGL task can hold the lock indefinitely if it is
 * stuck waiting for an LCD transfer that never reports completion -- which
 * has actually happened here. A bounded wait turns "this task is gone" into
 * "this update was skipped", which the stall watchdog can then act on. */
#define UI_LOCK_TIMEOUT_MS 1000

/* Draw buffer height in lines. Full-frame double buffering was extravagant:
 * LVGL only ever renders the invalidated area, so the extra size bought
 * nothing while a full-screen flush became a single 150KB transfer. Forty
 * lines covers the tallest element on this layout (the verdict banner) in
 * one pass. */
#define DRAW_BUF_LINES 40

static void log_internal_heap(const char *stage)
{
    ESP_LOGI(TAG, "internal RAM %s: %u free, largest block %u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void log_lock_timeout(const char *what)
{
    ESP_LOGE(TAG, "LVGL lock timed out in %s -- the LVGL task is not releasing it", what);
}

static lv_obj_t *s_place_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_banner;
static lv_obj_t *s_banner_label;
static lv_obj_t *s_summary1;
static lv_obj_t *s_summary2;
static lv_obj_t *s_nowcast;
static lv_obj_t *s_graph_caption;
static lv_obj_t *s_graph_area;
static lv_obj_t *s_pop_bars[MAX_BARS];
static lv_obj_t *s_mm_bars[MAX_BARS];
static lv_obj_t *s_axis_labels[MAX_AXIS_LABELS];

/* Weather icon, drawn from primitives rather than glyphs so it can carry
 * colour and move. */
static lv_obj_t *s_icon_box;
static lv_obj_t *s_sun_halo;
static lv_obj_t *s_sun_core;
static lv_obj_t *s_cloud_parts[4];
static lv_obj_t *s_drops[N_DROPS];
static bool      s_rain_animating;

static int         s_range_hours = 24;
static ui_tap_cb_t s_tap_cb;
static verdict_t   s_verdict = V_UNKNOWN;
static bool        s_night_theme;

static const char *verdict_text(verdict_t v)
{
    switch (v) {
        case V_OK:       return "ほしてだいじょうぶ";
        case V_CAUTION:  return "みじかめならOK";
        case V_BRING_IN: return "そろそろとりこもう";
        case V_RAINING:  return "あめ！とりこんで";
        default:         return "じゅんびちゅう";
    }
}

static uint32_t verdict_bg(verdict_t v)
{
    switch (v) {
        case V_OK:       return s_pal->ok_bg;
        case V_CAUTION:  return s_pal->caution_bg;
        case V_BRING_IN: return s_pal->bring_in_bg;
        case V_RAINING:  return s_pal->raining_bg;
        default:         return s_pal->unknown_bg;
    }
}

static uint32_t verdict_fg(verdict_t v)
{
    switch (v) {
        case V_OK:       return s_pal->ok_fg;
        case V_CAUTION:  return s_pal->caution_fg;
        case V_BRING_IN: return s_pal->bring_in_fg;
        case V_RAINING:  return s_pal->raining_fg;
        default:         return s_pal->unknown_fg;
    }
}

static void screen_tap_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (indev != NULL) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        ESP_LOGI(TAG, "tap at (%d, %d)", (int)p.x, (int)p.y);
    }

    s_range_hours = (s_range_hours == 24) ? 12 : 24;
    ESP_LOGI(TAG, "graph range -> %dh", s_range_hours);

    if (s_tap_cb != NULL) {
        s_tap_cb(s_range_hours);
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

/* A plain filled shape with none of lv_obj's default chrome. */
static lv_obj_t *make_shape(lv_obj_t *parent, int w, int h, int radius, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_icon(lv_obj_t *parent)
{
    s_icon_box = lv_obj_create(parent);
    lv_obj_set_size(s_icon_box, ICON_BOX, ICON_BOX);
    lv_obj_set_pos(s_icon_box, ICON_X, (BANNER_H - ICON_BOX) / 2);
    lv_obj_set_style_bg_opa(s_icon_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_icon_box, 0, 0);
    lv_obj_set_style_pad_all(s_icon_box, 0, 0);
    /* Drops animate past the cloud and must be clipped to the box. */
    lv_obj_remove_flag(s_icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_icon_box, LV_OBJ_FLAG_CLICKABLE);

    s_sun_halo = make_shape(s_icon_box, 44, 44, 22, s_pal->sun_halo);
    s_sun_core = make_shape(s_icon_box, 30, 30, 15, s_pal->sun);

    /* Cloud: three lobes over a flat base. */
    s_cloud_parts[0] = make_shape(s_icon_box, 40, 15, 8, s_pal->cloud);
    s_cloud_parts[1] = make_shape(s_icon_box, 20, 20, 10, s_pal->cloud);
    s_cloud_parts[2] = make_shape(s_icon_box, 26, 26, 13, s_pal->cloud_light);
    s_cloud_parts[3] = make_shape(s_icon_box, 18, 18, 9, s_pal->cloud);

    for (int i = 0; i < N_DROPS; i++) {
        s_drops[i] = make_shape(s_icon_box, 4, 9, 2, s_pal->drop);
        lv_obj_add_flag(s_drops[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void stop_rain(void)
{
    for (int i = 0; i < N_DROPS; i++) {
        lv_anim_delete(s_drops[i], NULL);
        lv_obj_add_flag(s_drops[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_rain_animating = false;
}

static void start_rain(void)
{
    if (s_rain_animating) {
        return;
    }
    for (int i = 0; i < N_DROPS; i++) {
        lv_obj_set_x(s_drops[i], 12 + i * 12);
        lv_obj_remove_flag(s_drops[i], LV_OBJ_FLAG_HIDDEN);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_drops[i]);
        lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
        lv_anim_set_values(&a, 34, ICON_BOX - 6);
        lv_anim_set_duration(&a, DROP_FALL_MS);
        /* Staggered so the drops read as rain rather than as a row of bars
         * moving in lockstep. */
        lv_anim_set_delay(&a, i * (DROP_FALL_MS / N_DROPS));
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
    s_rain_animating = true;
}

static void set_icon(verdict_t v)
{
    const bool show_sun   = (v == V_OK || v == V_CAUTION);
    const bool show_cloud = (v != V_OK);
    const bool show_rain  = (v == V_RAINING);

    if (show_sun) {
        /* Alone the sun sits centred; with a cloud it peeks out top-right. */
        const bool alone = (v == V_OK);
        lv_obj_set_pos(s_sun_halo, alone ? 4 : 16, alone ? 4 : 0);
        lv_obj_set_size(s_sun_halo, alone ? 44 : 32, alone ? 44 : 32);
        lv_obj_set_style_radius(s_sun_halo, alone ? 22 : 16, 0);
        lv_obj_set_pos(s_sun_core, alone ? 11 : 22, alone ? 11 : 6);
        lv_obj_set_size(s_sun_core, alone ? 30 : 20, alone ? 30 : 20);
        lv_obj_set_style_radius(s_sun_core, alone ? 15 : 10, 0);
        lv_obj_remove_flag(s_sun_halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sun_core, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sun_halo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sun_core, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_cloud) {
        const int base_y = show_rain ? 20 : 22;
        lv_obj_set_pos(s_cloud_parts[0], 5,  base_y + 6);
        lv_obj_set_pos(s_cloud_parts[1], 4,  base_y - 2);
        lv_obj_set_pos(s_cloud_parts[2], 14, base_y - 8);
        lv_obj_set_pos(s_cloud_parts[3], 29, base_y - 1);
        for (int i = 0; i < 4; i++) {
            lv_obj_remove_flag(s_cloud_parts[i], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        for (int i = 0; i < 4; i++) {
            lv_obj_add_flag(s_cloud_parts[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (show_rain) {
        start_rain();
    } else {
        stop_rain();
    }
}

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(s_pal->bg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    /* --- header --- */
    s_place_label = make_label(scr, &lw_font_jp_16, s_pal->text, PAD_X, HEADER_Y);
    s_status_label = make_label(scr, &lw_font_jp_16, s_pal->dim, 0, HEADER_Y);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_status_label, SCREEN_W / 2);
    lv_obj_set_pos(s_status_label, SCREEN_W - PAD_X - SCREEN_W / 2, HEADER_Y);

    /* --- verdict banner --- */
    s_banner = lv_obj_create(scr);
    lv_obj_set_size(s_banner, GRAPH_W, BANNER_H);
    lv_obj_set_pos(s_banner, PAD_X, BANNER_Y);
    lv_obj_set_style_radius(s_banner, 14, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(s_pal->unknown_bg), 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    /* Taps must reach the screen handler, not stop at the banner. */
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);

    build_icon(s_banner);

    s_banner_label = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_label, &lw_font_jp_24, 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_hex(s_pal->unknown_fg), 0);
    lv_obj_set_style_text_align(s_banner_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_banner_label, GRAPH_W - BANNER_TEXT_X - 6);
    lv_obj_set_pos(s_banner_label, BANNER_TEXT_X, (BANNER_H - 26) / 2);
    lv_label_set_text(s_banner_label, verdict_text(V_UNKNOWN));

    /* --- summary --- */
    s_summary1 = make_label(scr, &lw_font_jp_16, s_pal->text, PAD_X, SUMMARY1_Y);
    s_summary2 = make_label(scr, &lw_font_jp_16, s_pal->text, PAD_X, SUMMARY2_Y);

    /* --- graph --- */
    s_graph_caption = make_label(scr, &lw_font_jp_16, s_pal->dim, PAD_X, CAPTION_Y);

    s_nowcast = make_label(scr, &lw_font_jp_16, s_pal->dim, 0, CAPTION_Y);
    lv_obj_set_style_text_align(s_nowcast, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_nowcast, SCREEN_W / 2);
    lv_obj_set_pos(s_nowcast, SCREEN_W - PAD_X - SCREEN_W / 2, CAPTION_Y);
    lv_obj_add_flag(s_nowcast, LV_OBJ_FLAG_HIDDEN);

    s_graph_area = lv_obj_create(scr);
    lv_obj_set_size(s_graph_area, GRAPH_W, GRAPH_H);
    lv_obj_set_pos(s_graph_area, GRAPH_X, GRAPH_Y);
    lv_obj_set_style_bg_color(s_graph_area, lv_color_hex(s_pal->card), 0);
    lv_obj_set_style_border_color(s_graph_area, lv_color_hex(s_pal->border), 0);
    lv_obj_set_style_border_width(s_graph_area, 1, 0);
    lv_obj_set_style_radius(s_graph_area, 6, 0);
    lv_obj_set_style_pad_all(s_graph_area, 0, 0);
    lv_obj_remove_flag(s_graph_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_graph_area, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < MAX_BARS; i++) {
        s_pop_bars[i] = make_shape(s_graph_area, 1, 1, 1, s_pal->pop_bar);
        s_mm_bars[i]  = make_shape(s_graph_area, 1, 1, 1, s_pal->mm_bar);
    }

    for (int i = 0; i < MAX_AXIS_LABELS; i++) {
        s_axis_labels[i] = make_label(scr, &lw_font_jp_16, s_pal->dim, 0, AXIS_Y);
        lv_obj_add_flag(s_axis_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    set_icon(V_UNKNOWN);
}

/* Re-colours every persistent object. The bar colours are also set on each
 * graph update, but doing them here too avoids a stale-looking minute
 * between a theme change and the next refresh. */
static void apply_palette(void)
{
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(s_pal->bg), 0);

    lv_obj_set_style_text_color(s_place_label, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(s_pal->dim), 0);
    lv_obj_set_style_text_color(s_summary1, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_color(s_summary2, lv_color_hex(s_pal->text), 0);
    lv_obj_set_style_text_color(s_nowcast, lv_color_hex(s_pal->dim), 0);
    lv_obj_set_style_text_color(s_graph_caption, lv_color_hex(s_pal->dim), 0);

    lv_obj_set_style_bg_color(s_banner, lv_color_hex(verdict_bg(s_verdict)), 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_hex(verdict_fg(s_verdict)), 0);

    lv_obj_set_style_bg_color(s_graph_area, lv_color_hex(s_pal->card), 0);
    lv_obj_set_style_border_color(s_graph_area, lv_color_hex(s_pal->border), 0);

    for (int i = 0; i < MAX_BARS; i++) {
        lv_obj_set_style_bg_color(s_pop_bars[i], lv_color_hex(s_pal->pop_bar), 0);
        lv_obj_set_style_bg_color(s_mm_bars[i], lv_color_hex(s_pal->mm_bar), 0);
    }
    for (int i = 0; i < MAX_AXIS_LABELS; i++) {
        lv_obj_set_style_text_color(s_axis_labels[i], lv_color_hex(s_pal->dim), 0);
    }

    lv_obj_set_style_bg_color(s_sun_halo, lv_color_hex(s_pal->sun_halo), 0);
    lv_obj_set_style_bg_color(s_sun_core, lv_color_hex(s_pal->sun), 0);
    lv_obj_set_style_bg_color(s_cloud_parts[0], lv_color_hex(s_pal->cloud), 0);
    lv_obj_set_style_bg_color(s_cloud_parts[1], lv_color_hex(s_pal->cloud), 0);
    lv_obj_set_style_bg_color(s_cloud_parts[2], lv_color_hex(s_pal->cloud_light), 0);
    lv_obj_set_style_bg_color(s_cloud_parts[3], lv_color_hex(s_pal->cloud), 0);
    for (int i = 0; i < N_DROPS; i++) {
        lv_obj_set_style_bg_color(s_drops[i], lv_color_hex(s_pal->drop), 0);
    }
}

void ui_set_theme(bool night)
{
    if (night == s_night_theme) {
        return;
    }
    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_theme");
        return;
    }
    s_night_theme = night;
    s_pal = night ? &PALETTE_NIGHT : &PALETTE_DAY;
    apply_palette();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "theme -> %s", night ? "night" : "day");
}

bool ui_is_night_theme(void)
{
    return s_night_theme;
}

esp_err_t ui_start(esp_lcd_panel_handle_t panel,
                   esp_lcd_panel_io_handle_t io,
                   esp_lcd_touch_handle_t touch)
{
    ESP_RETURN_ON_FALSE(panel != NULL && io != NULL, ESP_ERR_INVALID_ARG, TAG, "no panel");

    log_internal_heap("ui_start entry");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");
    log_internal_heap("after lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        /* Partial buffers, in PSRAM. Internal RAM is the scarce resource on
         * this board and Wi-Fi needs tens of kilobytes of it after this
         * runs, so nothing here may be sized "because there is room". */
        .buffer_size   = SCREEN_W * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres          = SCREEN_W,
        .vres          = SCREEN_H,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            /* The panel takes RGB565 big-endian. This flag is LVGL v9's
             * replacement for the LV_COLOR_16_SWAP option SPEC §6 mentions,
             * which no longer exists. */
            .swap_bytes  = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(disp != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp failed");
    log_internal_heap("after add_disp");

    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "touch registration failed; display stays usable");
        }
    }

    log_internal_heap("after add_touch");

    if (lvgl_port_lock(0)) {
        build_screen();
        lvgl_port_unlock();
    } else {
        return ESP_ERR_TIMEOUT;
    }
    log_internal_heap("after build_screen");

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void ui_set_tap_cb(ui_tap_cb_t cb)
{
    s_tap_cb = cb;
}

void ui_set_header(const char *place, const char *status)
{
    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_header");
        return;
    }
    if (place != NULL) {
        lv_label_set_text(s_place_label, place);
    }
    if (status != NULL) {
        lv_label_set_text(s_status_label, status);
    }
    lvgl_port_unlock();
}

void ui_set_verdict(verdict_t v, const char *text)
{
    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_verdict");
        return;
    }
    s_verdict = v;
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(verdict_bg(v)), 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_hex(verdict_fg(v)), 0);
    lv_label_set_text(s_banner_label, (text != NULL) ? text : verdict_text(v));
    set_icon(v);
    lvgl_port_unlock();
}

void ui_set_summary(const char *line1, const char *line2)
{
    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_summary");
        return;
    }
    lv_label_set_text(s_summary1, line1 != NULL ? line1 : "");
    lv_label_set_text(s_summary2, line2 != NULL ? line2 : "");
    lvgl_port_unlock();
}

void ui_set_nowcast(float max_mm_h)
{
    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_nowcast");
        return;
    }
    if (max_mm_h < 0.0f) {
        lv_obj_add_flag(s_nowcast, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "レーダー %.1fmm/h", (double)max_mm_h);
        lv_label_set_text(s_nowcast, buf);
        lv_obj_remove_flag(s_nowcast, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

int ui_range_hours(void)
{
    return s_range_hours;
}

void ui_set_range_hours(int hours)
{
    s_range_hours = (hours == 12) ? 12 : 24;
}

void ui_set_graph(const hour_slot_t *slots, int n_slots, int start,
                  const judge_config_t *cfg)
{
    if (slots == NULL || cfg == NULL || start < 0) {
        return;
    }

    const int hours = s_range_hours;
    int shown = hours;
    if (start + shown > n_slots) {
        shown = n_slots - start;
    }
    if (shown <= 0) {
        return;
    }

    /* Amount is scaled against the largest value on screen, with a 1.0mm
     * floor so a drizzle-only day does not look like a downpour. */
    float max_mm = 1.0f;
    for (int i = 0; i < shown; i++) {
        if (slots[start + i].mm > max_mm) {
            max_mm = slots[start + i].mm;
        }
    }

    const int bar_pitch = GRAPH_W / hours;
    const int bar_w = (bar_pitch > 2) ? bar_pitch - 1 : 1;

    if (!lvgl_port_lock(UI_LOCK_TIMEOUT_MS)) {
        log_lock_timeout("ui_set_graph");
        return;
    }

    char caption[64];
    snprintf(caption, sizeof(caption), "さいだい %.1fmm/h ／ %d時間", (double)max_mm, hours);
    lv_label_set_text(s_graph_caption, caption);

    for (int i = 0; i < MAX_BARS; i++) {
        if (i >= shown) {
            lv_obj_add_flag(s_pop_bars[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_mm_bars[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        const hour_slot_t *slot = &slots[start + i];
        const int x = i * bar_pitch;

        int pop_h = (slot->pop * GRAPH_H) / 100;
        if (pop_h < 0) { pop_h = 0; }
        if (pop_h > GRAPH_H) { pop_h = GRAPH_H; }

        int mm_h = (int)((slot->mm / max_mm) * (float)GRAPH_H);
        if (mm_h > GRAPH_H) { mm_h = GRAPH_H; }
        /* Keep any non-zero rainfall visible instead of rounding it away. */
        if (slot->mm > 0.0f && mm_h < 2) { mm_h = 2; }
        if (mm_h < 0) { mm_h = 0; }

        lv_obj_set_size(s_pop_bars[i], bar_w, pop_h > 0 ? pop_h : 1);
        lv_obj_set_pos(s_pop_bars[i], x, GRAPH_H - (pop_h > 0 ? pop_h : 1));
        lv_obj_set_style_bg_opa(s_pop_bars[i], pop_h > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_pop_bars[i], LV_OBJ_FLAG_HIDDEN);

        const bool rainy = judge_is_rainy(slot, cfg);
        lv_obj_set_style_bg_color(s_mm_bars[i],
                                  lv_color_hex(rainy ? s_pal->mm_rainy : s_pal->mm_bar), 0);
        lv_obj_set_size(s_mm_bars[i], bar_w, mm_h > 0 ? mm_h : 1);
        lv_obj_set_pos(s_mm_bars[i], x, GRAPH_H - (mm_h > 0 ? mm_h : 1));
        lv_obj_set_style_bg_opa(s_mm_bars[i], mm_h > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(s_mm_bars[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* Hour labels every three slots, using local time. */
    int label_i = 0;
    for (int i = 0; i < shown && label_i < MAX_AXIS_LABELS; i += AXIS_LABEL_STEP) {
        struct tm tm_local;
        const time_t t = slots[start + i].t;
        localtime_r(&t, &tm_local);

        char buf[8];
        snprintf(buf, sizeof(buf), "%d", tm_local.tm_hour);
        lv_label_set_text(s_axis_labels[label_i], buf);
        lv_obj_set_pos(s_axis_labels[label_i], GRAPH_X + i * bar_pitch, AXIS_Y);
        lv_obj_remove_flag(s_axis_labels[label_i], LV_OBJ_FLAG_HIDDEN);
        label_i++;
    }
    for (; label_i < MAX_AXIS_LABELS; label_i++) {
        lv_obj_add_flag(s_axis_labels[label_i], LV_OBJ_FLAG_HIDDEN);
    }

    lvgl_port_unlock();
}
