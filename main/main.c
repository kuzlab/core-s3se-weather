/* Laundry weather display for M5Stack CoreS3 SE.
 *
 * Two tasks share one mutex-protected state (SPEC §3.2): net_task fetches
 * and judges, ui_task renders. LVGL is only ever touched from ui_task and
 * from LVGL's own task, never from the network path.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "cores3se_bsp.h"
#include "judge.h"
#include "time_sync.h"
#include "ui.h"
#include "weather_openmeteo.h"
#include "wifi.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define NET_TASK_STACK 8192
#define UI_TASK_STACK  8192
#define NET_TASK_PRIO  5
#define UI_TASK_PRIO   4

#define UI_TICK_MS     60000   /* re-render at least once a minute */
#define HEAP_LOG_SEC   300

static const judge_config_t CFG = {
    .rain_mm    = CONFIG_LW_RAIN_MM_X100 / 100.0f,
    .rain_pop   = CONFIG_LW_RAIN_POP,
    .bring_in_h = CONFIG_LW_BRING_IN_H,
    .caution_h  = CONFIG_LW_CAUTION_H,
};

static TaskHandle_t s_net_task;
static TaskHandle_t s_ui_task;

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* ---------------------------------------------------------------- net --- */

static void net_task(void *arg)
{
    bm8563_handle_t rtc = bsp_bm8563();

    /* The first association can take a while on a busy channel; the retry
     * loop keeps trying regardless of what this wait returns. */
    wifi_wait_connected(30000);
    time_sync_start_sntp();

    bool rtc_written = false;
    int64_t last_fetch_us = 0;

    while (true) {
        const bool due = (last_fetch_us == 0) ||
                         (esp_timer_get_time() - last_fetch_us >=
                          (int64_t)CONFIG_LW_REFRESH_SEC * 1000000);

        if (due && wifi_is_connected()) {
            if (!time_is_valid()) {
                /* A judgement made against an unset clock would place "now"
                 * in the wrong slot, so skip the whole cycle (SPEC §8.2). */
                ESP_LOGW(TAG, "clock not synced yet; deferring fetch");
            } else {
                if (!rtc_written && rtc != NULL) {
                    rtc_written = (time_sync_store_to_rtc(rtc) == ESP_OK);
                }

                openmeteo_result_t res;
                const esp_err_t err = openmeteo_fetch(&res);
                last_fetch_us = esp_timer_get_time();

                if (app_state_lock(2000)) {
                    app_state_t *st = app_state_locked();
                    st->last_fetch_ok = (err == ESP_OK);
                    if (err == ESP_OK) {
                        memcpy(st->slots, res.slots, sizeof(res.slots));
                        st->n_slots = res.n_slots;
                        st->judge = judge_evaluate(st->slots, st->n_slots, time(NULL), &CFG);
                        judge_apply_nowcast(&st->judge, st->nowcast_max_mm_h,
                                            CONFIG_LW_NOWCAST_MM_H_X100 / 100.0f);
                        st->last_update = time(NULL);
                        ESP_LOGI(TAG, "verdict=%s hours_to_rain=%d total=%.1fmm peak_pop=%d%%",
                                 judge_verdict_label(st->judge.verdict),
                                 st->judge.hours_to_rain,
                                 (double)st->judge.rain_total_mm,
                                 st->judge.rain_peak_pop);
                    } else {
                        /* Keep the previous slots on screen. Only the header
                         * changes, to show the last success (SPEC §8.1). */
                        ESP_LOGW(TAG, "fetch failed; keeping previous data");
                    }
                    app_state_unlock();
                }
                xTaskNotifyGive(s_ui_task);
            }
        }

        /* Wake early when a tap asks for an immediate refetch. */
        const uint32_t wait_ms = due ? 5000 : 1000;
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms)) > 0) {
            ESP_LOGI(TAG, "refetch requested");
            last_fetch_us = 0;
        }
    }
}

/* ----------------------------------------------------------------- ui --- */

static void format_summary(const app_state_t *st, char *l1, size_t l1_len,
                           char *l2, size_t l2_len)
{
    l1[0] = '\0';
    l2[0] = '\0';

    if (st->judge.verdict == V_UNKNOWN) {
        snprintf(l1, l1_len, "時刻待ち");
        return;
    }

    if (st->judge.hours_to_rain < 0) {
        snprintf(l1, l1_len, "24時間以内に雨の予報はありません");
        return;
    }

    struct tm tm_s, tm_e;
    localtime_r(&st->judge.rain_start, &tm_s);
    localtime_r(&st->judge.rain_end, &tm_e);
    snprintf(l1, l1_len, "%02d:%02d〜%02d:%02d  %.1fmm  最大%d%%",
             tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min,
             (double)st->judge.rain_total_mm, st->judge.rain_peak_pop);

    if (st->judge.hours_to_rain == 0) {
        snprintf(l2, l2_len, "今降っています");
    } else {
        snprintf(l2, l2_len, "あと約%d時間で降り出します", st->judge.hours_to_rain);
    }
}

static void apply_night_dimming(void)
{
    static int last_brightness = -1;

    int brightness = CONFIG_LW_BRIGHTNESS_DAY;
    if (time_is_valid()) {
        const time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);

        const int h = local.tm_hour;
        const int start = CONFIG_LW_NIGHT_START_HOUR;
        const int end   = CONFIG_LW_NIGHT_END_HOUR;
        /* The night window normally wraps past midnight, so it is two
         * ranges rather than one comparison. */
        const bool night = (start <= end) ? (h >= start && h < end)
                                          : (h >= start || h < end);
        if (night) {
            brightness = CONFIG_LW_BRIGHTNESS_NIGHT;
        }
    }

    if (brightness != last_brightness) {
        bsp_backlight_set((uint8_t)brightness);
        ESP_LOGI(TAG, "backlight -> %d", brightness);
        last_brightness = brightness;
    }
}

static void render(void)
{
    app_state_t st;
    app_state_get(&st);

    char status[32];
    if (st.last_update == 0) {
        snprintf(status, sizeof(status), wifi_is_connected() ? "取得中" : "接続中");
    } else {
        struct tm tm_u;
        localtime_r(&st.last_update, &tm_u);
        /* The timestamp always shows the last *success*, with the prefix
         * saying whether the newest attempt worked (SPEC §8.1). */
        snprintf(status, sizeof(status), "%s %02d:%02d",
                 st.last_fetch_ok ? "更新" : "失敗", tm_u.tm_hour, tm_u.tm_min);
    }
    ui_set_header(CONFIG_LW_PLACE_NAME, status);

    ui_set_verdict(st.judge.verdict);

    char l1[80];
    char l2[80];
    format_summary(&st, l1, sizeof(l1), l2, sizeof(l2));
    ui_set_summary(l1, l2);

    ui_set_nowcast(st.nowcast_max_mm_h);

    if (st.n_slots > 0 && time_is_valid()) {
        /* Find the slot holding "now" so the graph starts at the current
         * hour rather than at whatever the forecast begins with. */
        const time_t now = time(NULL);
        int start = -1;
        for (int i = 0; i < st.n_slots; i++) {
            if (now >= st.slots[i].t && now < st.slots[i].t + JUDGE_SLOT_SEC) {
                start = i;
                break;
            }
        }
        if (start >= 0) {
            ui_set_graph(st.slots, st.n_slots, start, &CFG);
        }
    }

    apply_night_dimming();
}

static void ui_task(void *arg)
{
    int64_t last_heap_log_us = 0;

    while (true) {
        render();

        if (esp_timer_get_time() - last_heap_log_us >= (int64_t)HEAP_LOG_SEC * 1000000) {
            ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes, min ever: %" PRIu32
                          ", uptime: %" PRId64 " s",
                     esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
                     esp_timer_get_time() / 1000000);
            last_heap_log_us = esp_timer_get_time();
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_TICK_MS));
    }
}

static void on_tap(int new_range_hours)
{
    /* Runs inside LVGL's event handler, so it only signals: the refetch and
     * the redraw happen on their own tasks. */
    if (s_net_task != NULL) {
        xTaskNotifyGive(s_net_task);
    }
    if (s_ui_task != NULL) {
        xTaskNotifyGive(s_ui_task);
    }
}

/* --------------------------------------------------------------- main --- */

void app_main(void)
{
    ESP_LOGI(TAG, "laundry-weather starting (%s)", CONFIG_LW_PLACE_NAME);

    init_nvs();
    ESP_ERROR_CHECK(app_state_init());
    ESP_ERROR_CHECK(bsp_init());

    /* TZ and the provisional clock come from the RTC before the network is
     * up, so the header is not stuck on 1970 (SPEC §8.2). */
    time_sync_init(bsp_bm8563());

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

    if (wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi could not start; the display will show no data");
    }

    xTaskCreate(ui_task, "ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, &s_ui_task);
    xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIO, &s_net_task);

    ESP_LOGI(TAG, "tasks started");
}
