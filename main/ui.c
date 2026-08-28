#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lw_fonts.h"
#include "sdkconfig.h"

static const char *TAG = "ui";

#define SCREEN_W 320
#define SCREEN_H 240

/* Layout, top to bottom. Kept as constants rather than a layout engine
 * because the screen is fixed-size and every element has a set place. */
#define PAD_X            6
#define HEADER_Y         2
#define HEADER_H         18
#define BANNER_Y         22
#define BANNER_H         42
#define SUMMARY1_Y       68
#define SUMMARY2_Y       88
#define NOWCAST_Y        108
#define GRAPH_CAPTION_Y  128
#define GRAPH_Y          146
#define GRAPH_H          56
#define AXIS_Y           204

#define GRAPH_X          PAD_X
#define GRAPH_W          (SCREEN_W - 2 * PAD_X)

/* SPEC §5 verdict colours */
#define COLOR_OK        0x2EA043
#define COLOR_CAUTION   0xD89B00
#define COLOR_BRING_IN  0xE05000
#define COLOR_RAINING   0xC02030
#define COLOR_UNKNOWN   0x404850

#define COLOR_BG        0x101418
#define COLOR_TEXT      0xE6EDF3
#define COLOR_DIM       0x8B949E
#define COLOR_POP_BAR   0x1F3A5F  /* probability, behind */
#define COLOR_MM_BAR    0x39D0D8  /* amount, in front */
#define COLOR_MM_RAINY  0x58A6FF  /* amount in a slot judged rainy */

/* Bars are pre-created for the widest range so switching 24h <-> 12h only
 * resizes them; creating and deleting objects on every update would churn
 * LVGL's heap for no reason. */
#define MAX_BARS         24
#define AXIS_LABEL_STEP  3
#define MAX_AXIS_LABELS  (MAX_BARS / AXIS_LABEL_STEP + 1)

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

static int         s_range_hours = 24;
static ui_tap_cb_t s_tap_cb;

static uint32_t verdict_color(verdict_t v)
{
    switch (v) {
        case V_OK:       return COLOR_OK;
        case V_CAUTION:  return COLOR_CAUTION;
        case V_BRING_IN: return COLOR_BRING_IN;
        case V_RAINING:  return COLOR_RAINING;
        default:         return COLOR_UNKNOWN;
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
        /* The callback triggers a refetch, so it must not run inside the
         * LVGL lock this event handler is already holding. It is expected to
         * only signal the network task. */
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

static void build_screen(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_tap_cb, LV_EVENT_CLICKED, NULL);

    /* --- header --- */
    s_place_label = make_label(scr, &lw_font_jp_16, COLOR_TEXT, PAD_X, HEADER_Y);
    s_status_label = make_label(scr, &lw_font_jp_16, COLOR_DIM, 0, HEADER_Y);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_status_label, SCREEN_W / 2);
    lv_obj_set_pos(s_status_label, SCREEN_W - PAD_X - SCREEN_W / 2, HEADER_Y);

    /* --- verdict banner --- */
    s_banner = lv_obj_create(scr);
    lv_obj_set_size(s_banner, GRAPH_W, BANNER_H);
    lv_obj_set_pos(s_banner, PAD_X, BANNER_Y);
    lv_obj_set_style_radius(s_banner, 6, 0);
    lv_obj_set_style_border_width(s_banner, 0, 0);
    lv_obj_set_style_pad_all(s_banner, 0, 0);
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(COLOR_UNKNOWN), 0);
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_SCROLLABLE);
    /* Taps must reach the screen handler, not stop at the banner. */
    lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_CLICKABLE);

    s_banner_label = lv_label_create(s_banner);
    lv_obj_set_style_text_font(s_banner_label, &lw_font_jp_24, 0);
    lv_obj_set_style_text_color(s_banner_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_banner_label, "起動中");
    lv_obj_center(s_banner_label);

    /* --- summary --- */
    s_summary1 = make_label(scr, &lw_font_jp_16, COLOR_TEXT, PAD_X, SUMMARY1_Y);
    s_summary2 = make_label(scr, &lw_font_jp_16, COLOR_TEXT, PAD_X, SUMMARY2_Y);
    s_nowcast  = make_label(scr, &lw_font_jp_16, COLOR_DIM,  PAD_X, NOWCAST_Y);
    lv_obj_add_flag(s_nowcast, LV_OBJ_FLAG_HIDDEN);

    /* --- graph --- */
    s_graph_caption = make_label(scr, &lw_font_jp_16, COLOR_DIM, PAD_X, GRAPH_CAPTION_Y);

    s_graph_area = lv_obj_create(scr);
    lv_obj_set_size(s_graph_area, GRAPH_W, GRAPH_H);
    lv_obj_set_pos(s_graph_area, GRAPH_X, GRAPH_Y);
    lv_obj_set_style_bg_color(s_graph_area, lv_color_hex(0x161B22), 0);
    lv_obj_set_style_border_width(s_graph_area, 0, 0);
    lv_obj_set_style_radius(s_graph_area, 2, 0);
    lv_obj_set_style_pad_all(s_graph_area, 0, 0);
    lv_obj_remove_flag(s_graph_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_graph_area, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < MAX_BARS; i++) {
        s_pop_bars[i] = lv_obj_create(s_graph_area);
        lv_obj_set_style_bg_color(s_pop_bars[i], lv_color_hex(COLOR_POP_BAR), 0);
        lv_obj_set_style_border_width(s_pop_bars[i], 0, 0);
        lv_obj_set_style_radius(s_pop_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_pop_bars[i], 0, 0);
        lv_obj_remove_flag(s_pop_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_pop_bars[i], LV_OBJ_FLAG_CLICKABLE);

        s_mm_bars[i] = lv_obj_create(s_graph_area);
        lv_obj_set_style_bg_color(s_mm_bars[i], lv_color_hex(COLOR_MM_BAR), 0);
        lv_obj_set_style_border_width(s_mm_bars[i], 0, 0);
        lv_obj_set_style_radius(s_mm_bars[i], 0, 0);
        lv_obj_set_style_pad_all(s_mm_bars[i], 0, 0);
        lv_obj_remove_flag(s_mm_bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_mm_bars[i], LV_OBJ_FLAG_CLICKABLE);
    }

    for (int i = 0; i < MAX_AXIS_LABELS; i++) {
        s_axis_labels[i] = make_label(scr, &lw_font_jp_16, COLOR_DIM, 0, AXIS_Y);
        lv_obj_add_flag(s_axis_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

esp_err_t ui_start(esp_lcd_panel_handle_t panel,
                   esp_lcd_panel_io_handle_t io,
                   esp_lcd_touch_handle_t touch)
{
    ESP_RETURN_ON_FALSE(panel != NULL && io != NULL, ESP_ERR_INVALID_ARG, TAG, "no panel");

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        /* Full-frame buffers in PSRAM: there is 8MB and only ~300KB is
         * needed, so there is no reason to redraw in narrow strips. */
        .buffer_size   = SCREEN_W * SCREEN_H,
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

    if (touch != NULL) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp   = disp,
            .handle = touch,
        };
        if (lvgl_port_add_touch(&touch_cfg) == NULL) {
            ESP_LOGW(TAG, "touch registration failed; display stays usable");
        }
    }

    if (lvgl_port_lock(0)) {
        build_screen();
        lvgl_port_unlock();
    } else {
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void ui_set_tap_cb(ui_tap_cb_t cb)
{
    s_tap_cb = cb;
}

void ui_set_header(const char *place, const char *status)
{
    if (!lvgl_port_lock(0)) {
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

void ui_set_verdict(verdict_t v)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(verdict_color(v)), 0);
    lv_label_set_text(s_banner_label, judge_verdict_label(v));
    lv_obj_center(s_banner_label);
    lvgl_port_unlock();
}

void ui_set_summary(const char *line1, const char *line2)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    lv_label_set_text(s_summary1, line1 != NULL ? line1 : "");
    lv_label_set_text(s_summary2, line2 != NULL ? line2 : "");
    lvgl_port_unlock();
}

void ui_set_nowcast(float max_mm_h)
{
    if (!lvgl_port_lock(0)) {
        return;
    }
    if (max_mm_h < 0.0f) {
        lv_obj_add_flag(s_nowcast, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "直近60分レーダー：最大 %.1f mm/h", (double)max_mm_h);
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

    if (!lvgl_port_lock(0)) {
        return;
    }

    char caption[64];
    snprintf(caption, sizeof(caption), "最大 %.1fmm/h ／ %d時間表示", (double)max_mm, hours);
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
                                  lv_color_hex(rainy ? COLOR_MM_RAINY : COLOR_MM_BAR), 0);
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
