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

#include "esp_heap_caps.h"
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

#define FETCH_RETRY_START_SEC 30
#define FETCH_RETRY_MAX_SEC   300

static const judge_config_t CFG = {
    .rain_mm    = CONFIG_LW_RAIN_MM_X100 / 100.0f,
    .rain_pop   = CONFIG_LW_RAIN_POP,
    .bring_in_h = CONFIG_LW_BRING_IN_H,
    .caution_h  = CONFIG_LW_CAUTION_H,
};

static TaskHandle_t s_net_task;
static TaskHandle_t s_ui_task;

/* Last verdict the user was alerted about. Starts at V_UNKNOWN so the very
 * first reading never beeps: at power-on nothing has changed yet, and a
 * device that greets you with an alarm is a device you unplug. */
static verdict_t s_alerted_verdict = V_UNKNOWN;

/* SPEC §1.4: beep only on the transition into a "bring it in" state, and
 * only from a state that was previously fine. Staying bad must stay silent. */
static bool verdict_worsened(verdict_t before, verdict_t after)
{
    const bool was_fine = (before == V_OK || before == V_CAUTION);
    const bool now_bad  = (after == V_BRING_IN || after == V_RAINING);
    return was_fine && now_bad;
}

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
    /* After a failure, retry well before the normal refresh interval --
     * waiting ten minutes to notice the network came back would make the
     * display stale for no reason. Backs off so a long outage does not mean
     * hammering the API (SPEC §8.1). */
    int interval_sec = CONFIG_LW_REFRESH_SEC;

    while (true) {
        const bool due = (last_fetch_us == 0) ||
                         (esp_timer_get_time() - last_fetch_us >=
                          (int64_t)interval_sec * 1000000);

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
                verdict_t new_verdict = V_UNKNOWN;
                const esp_err_t err = openmeteo_fetch(&res);
                last_fetch_us = esp_timer_get_time();

                if (err == ESP_OK) {
                    interval_sec = CONFIG_LW_REFRESH_SEC;
                } else {
                    interval_sec = (interval_sec >= CONFIG_LW_REFRESH_SEC)
                                 ? FETCH_RETRY_START_SEC
                                 : interval_sec * 2;
                    if (interval_sec > FETCH_RETRY_MAX_SEC) {
                        interval_sec = FETCH_RETRY_MAX_SEC;
                    }
                    ESP_LOGW(TAG, "retrying in %d s", interval_sec);
                }

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
                        new_verdict = st->judge.verdict;
                    } else {
                        /* Keep the previous slots on screen. Only the header
                         * changes, to show the last success (SPEC §8.1). */
                        ESP_LOGW(TAG, "fetch failed; keeping previous data");
                    }
                    app_state_unlock();
                }
                xTaskNotifyGive(s_ui_task);

#if CONFIG_LW_ENABLE_BEEP
                if (verdict_worsened(s_alerted_verdict, new_verdict)) {
                    ESP_LOGI(TAG, "verdict worsened %s -> %s; alerting",
                             judge_verdict_label(s_alerted_verdict),
                             judge_verdict_label(new_verdict));
                    bsp_beep_alert();
                }
#endif
                if (new_verdict != V_UNKNOWN) {
                    s_alerted_verdict = new_verdict;
                }
            }
        }

        /* Wake early when a tap asks for an immediate refetch. */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) > 0) {
            ESP_LOGI(TAG, "refetch requested");
            last_fetch_us = 0;
        }
    }
}

/* ----------------------------------------------------------------- ui --- */

/* When there is no verdict yet, the summary lines are the only place the
 * device can explain itself. Saying "接続中" forever tells the user nothing
 * about whether to fix the SSID, the password, or the placement, so name the
 * actual failure. */
static void format_startup_summary(char *l1, size_t l1_len, char *l2, size_t l2_len)
{
    switch (wifi_ui_state()) {
        case WIFI_UI_DISABLED:
            snprintf(l1, l1_len, "Wi-Fiにつながりません");
            snprintf(l2, l2_len, "SSIDが設定されていません");
            return;
        case WIFI_UI_NO_AP:
            snprintf(l1, l1_len, "Wi-Fiにつながりません");
            /* All three causes look identical from here, so name them all
             * rather than guessing at one. */
            snprintf(l2, l2_len, "%s が見つかりません", CONFIG_LW_WIFI_SSID);
            return;
        case WIFI_UI_BAD_AUTH:
            snprintf(l1, l1_len, "Wi-Fiにつながりません");
            snprintf(l2, l2_len, "パスワードが違うようです");
            return;
        case WIFI_UI_OTHER_FAILURE:
            snprintf(l1, l1_len, "Wi-Fiにつながりません");
            snprintf(l2, l2_len, "電波が届いていないようです");
            return;
        case WIFI_UI_CONNECTING:
            snprintf(l1, l1_len, "Wi-Fiにつないでいます");
            snprintf(l2, l2_len, "%s", CONFIG_LW_WIFI_SSID);
            return;
        case WIFI_UI_CONNECTED:
            break;
    }

    if (!time_is_valid()) {
        snprintf(l1, l1_len, "時刻をあわせています");
        return;
    }
    snprintf(l1, l1_len, "よほうをとりよせています");
}

static void format_summary(const app_state_t *st, char *l1, size_t l1_len,
                           char *l2, size_t l2_len)
{
    l1[0] = '\0';
    l2[0] = '\0';

    if (st->judge.verdict == V_UNKNOWN) {
        format_startup_summary(l1, l1_len, l2, l2_len);
        return;
    }

    if (st->judge.hours_to_rain < 0) {
        snprintf(l1, l1_len, "☀ %d時間 あめのよほうはなさそう", JUDGE_HORIZON_H);
        return;
    }

    struct tm tm_s, tm_e;
    localtime_r(&st->judge.rain_start, &tm_s);
    localtime_r(&st->judge.rain_end, &tm_e);
    snprintf(l1, l1_len, "☂ %02d:%02d〜%02d:%02d  %.1fmm  さいだい%d%%",
             tm_s.tm_hour, tm_s.tm_min, tm_e.tm_hour, tm_e.tm_min,
             (double)st->judge.rain_total_mm, st->judge.rain_peak_pop);

    if (st->judge.hours_to_rain == 0) {
        snprintf(l2, l2_len, "いまふっています");
    } else {
        snprintf(l2, l2_len, "あと%d時間くらいでふりそう", st->judge.hours_to_rain);
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
        /* Distinct words for distinct stages: a single "接続中" that never
         * changes is indistinguishable from a hang. */
        const char *stage;
        switch (wifi_ui_state()) {
            case WIFI_UI_CONNECTED:  stage = time_is_valid() ? "取得中" : "同期中"; break;
            case WIFI_UI_CONNECTING: stage = "接続中"; break;
            default:                 stage = "未接続"; break;
        }
        snprintf(status, sizeof(status), "%s", stage);
    } else {
        struct tm tm_u;
        localtime_r(&st.last_update, &tm_u);
        /* The timestamp always shows the last *success*, with the prefix
         * saying whether the newest attempt worked (SPEC §8.1). */
        snprintf(status, sizeof(status), "%s %02d:%02d",
                 st.last_fetch_ok ? "更新" : "更新できず", tm_u.tm_hour, tm_u.tm_min);
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
    /* Negative so the first pass logs immediately: waiting HEAP_LOG_SEC for
     * the first sample leaves the early minutes -- when allocation problems
     * actually show up -- with no record at all. */
    int64_t last_heap_log_us = -(int64_t)HEAP_LOG_SEC * 1000000;

    while (true) {
        render();

        if (esp_timer_get_time() - last_heap_log_us >= (int64_t)HEAP_LOG_SEC * 1000000) {
            /* Internal RAM is reported separately because the total is
             * dominated by 8MB of PSRAM: an internal-RAM shortage that
             * breaks TLS is invisible in the combined figure. The largest
             * free block matters more than the sum, since that is what an
             * allocation actually needs. */
            ESP_LOGI(TAG, "heap: total %u free / internal %u free, "
                          "largest internal block %u, min internal ever %u, uptime %" PRId64 " s",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
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

#if CONFIG_LW_ENABLE_BEEP
    /* Sound is a secondary feature; a failure here must not stop the
     * display, which is the actual product (SPEC §4.2). */
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "audio unavailable; verdict changes will be silent");
    }
#endif
    ESP_ERROR_CHECK(bsp_backlight_set(CONFIG_LW_BRIGHTNESS_DAY));

    if (wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi could not start; the display will show no data");
    }

    xTaskCreate(ui_task, "ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, &s_ui_task);
    xTaskCreate(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIO, &s_net_task);

    ESP_LOGI(TAG, "tasks started");
}
