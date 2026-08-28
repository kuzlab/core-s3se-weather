/* Laundry weather display for M5Stack CoreS3 SE.
 *
 * Milestones 1-4: board bring-up, LCD, LVGL with subset Japanese fonts, and
 * touch input. The weather data is still synthetic -- this build exists to
 * prove the screen renders and the panel responds to taps.
 *
 * The diagnostic block repeats on a timer rather than printing once at boot,
 * because this board accepts no software reset: the only way to restart it
 * is to unplug USB, and printing once would mean racing the log capture
 * against that replug.
 */
#include <inttypes.h>
#include <stdio.h>
#include <time.h>

#include "cores3se_bsp.h"
#include "judge.h"
#include "ui.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define DIAG_INTERVAL_MS 20000

static const judge_config_t CFG = {
    .rain_mm    = CONFIG_LW_RAIN_MM_X100 / 100.0f,
    .rain_pop   = CONFIG_LW_RAIN_POP,
    .bring_in_h = CONFIG_LW_BRING_IN_H,
    .caution_h  = CONFIG_LW_CAUTION_H,
};

static hour_slot_t s_slots[JUDGE_MAX_SLOTS];
static int         s_n_slots;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* Stand-in forecast until milestone 6 fetches the real thing: a dry morning,
 * a burst of rain a few hours out, then clearing. Shaped so every part of
 * the layout has something to show. */
static void make_placeholder_forecast(time_t now)
{
    const time_t base = (now / JUDGE_SLOT_SEC) * JUDGE_SLOT_SEC;
    s_n_slots = JUDGE_MAX_SLOTS;

    for (int i = 0; i < s_n_slots; i++) {
        s_slots[i].t   = base + (time_t)i * JUDGE_SLOT_SEC;
        s_slots[i].mm  = 0.0f;
        s_slots[i].pop = 5 + (i % 7) * 3;
    }
    s_slots[4].mm = 0.6f;  s_slots[4].pop = 60;
    s_slots[5].mm = 2.4f;  s_slots[5].pop = 80;
    s_slots[6].mm = 1.1f;  s_slots[6].pop = 70;
    s_slots[7].mm = 0.3f;  s_slots[7].pop = 55;
}

static void render_placeholder(time_t now)
{
    const judgement_t j = judge_evaluate(s_slots, s_n_slots, now, &CFG);

    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char status[32];
    snprintf(status, sizeof(status), "更新 %02d:%02d", tm_now.tm_hour, tm_now.tm_min);

    ui_set_header(CONFIG_LW_PLACE_NAME, status);
    ui_set_verdict(j.verdict);

    char line1[64] = "";
    char line2[64] = "";
    if (j.hours_to_rain >= 0) {
        struct tm tm_s, tm_e;
        localtime_r(&j.rain_start, &tm_s);
        localtime_r(&j.rain_end, &tm_e);
        snprintf(line1, sizeof(line1), "%02d:%02d〜%02d:%02d  %.1fmm  最大%d%%",
                 tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min,
                 (double)j.rain_total_mm, j.rain_peak_pop);
        if (j.hours_to_rain == 0) {
            snprintf(line2, sizeof(line2), "今降っています");
        } else {
            snprintf(line2, sizeof(line2), "あと約%d時間で降り出します", j.hours_to_rain);
        }
    } else {
        snprintf(line1, sizeof(line1), "24時間以内に雨の予報はありません");
    }
    ui_set_summary(line1, line2);
    ui_set_nowcast(-1.0f);

    /* Slot 0 is "now" in the placeholder data. */
    ui_set_graph(s_slots, s_n_slots, 0, &CFG);
}

static void on_tap(int new_range_hours)
{
    ESP_LOGI(TAG, "tap: range now %dh (refetch would happen here)", new_range_hours);
    render_placeholder(time(NULL));
}

void app_main(void)
{
    ESP_LOGI(TAG, "laundry-weather starting (%s)", CONFIG_LW_PLACE_NAME);

    /* JST for every localtime_r() in the app (SPEC §8.2). The clock itself
     * is not set until milestone 5, so times shown now are meaningless. */
    setenv("TZ", "JST-9", 1);
    tzset();

    init_nvs();

    ESP_ERROR_CHECK(bsp_init());

    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    ESP_ERROR_CHECK(bsp_display_init(&panel, &io));

    esp_lcd_touch_handle_t touch = NULL;
    if (bsp_touch_init(&touch) != ESP_OK) {
        ESP_LOGW(TAG, "touch unavailable; continuing without it");
        touch = NULL;
    }

    ESP_ERROR_CHECK(ui_start(panel, io, touch));
    ui_set_tap_cb(on_tap);

    ESP_ERROR_CHECK(bsp_backlight_set(CONFIG_LW_BRIGHTNESS_DAY));

    const time_t now = time(NULL);
    make_placeholder_forecast(now);
    render_placeholder(now);

    while (true) {
        ESP_LOGI(TAG, "===== diagnostics =====");
        bsp_i2c_scan_log();
        ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes, min ever: %" PRIu32
                      ", uptime: %" PRId64 " s",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
                 esp_timer_get_time() / 1000000);
        vTaskDelay(pdMS_TO_TICKS(DIAG_INTERVAL_MS));
    }
}
