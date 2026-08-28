#include "wifi.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0
#define BACKOFF_START_SEC  1

static EventGroupHandle_t s_events;
static volatile bool      s_connected;
static int                s_backoff_sec = BACKOFF_START_SEC;
static TaskHandle_t       s_retry_task;
static volatile uint8_t   s_last_reason;
static bool               s_scan_logged;
static volatile wifi_ui_state_t s_ui_state = WIFI_UI_DISABLED;

/* Lists what the radio can actually see. "AP not found" is ambiguous from
 * the outside -- a typo, a 5GHz-only network and being out of range all look
 * identical -- so dump the visible 2.4GHz networks instead of guessing. */
static void log_visible_aps(void)
{
    const wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGW(TAG, "scan found no networks at all");
        return;
    }
    if (n > 20) {
        n = 20;
    }

    wifi_ap_record_t *records = calloc(n, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        esp_wifi_clear_ap_list();
        return;
    }

    if (esp_wifi_scan_get_ap_records(&n, records) == ESP_OK) {
        ESP_LOGW(TAG, "--- visible 2.4GHz networks (%d) ---", n);
        for (int i = 0; i < n; i++) {
            ESP_LOGW(TAG, "  \"%s\"  rssi %d  ch %d  auth %d",
                     (const char *)records[i].ssid, records[i].rssi,
                     records[i].primary, records[i].authmode);
        }
        ESP_LOGW(TAG, "--- configured SSID is \"%s\" (comparison is case-sensitive) ---",
                 CONFIG_LW_WIFI_SSID);
    }
    free(records);
}

/* Reconnecting from the event handler would block the event loop for the
 * whole backoff, so the delay is served on its own task. */
static void retry_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const int delay = s_backoff_sec;
        ESP_LOGI(TAG, "reconnecting in %d s", delay);
        vTaskDelay(pdMS_TO_TICKS(delay * 1000));

        /* Scan once the failures stop looking transient, and only then:
         * a scan takes seconds and would slow down every ordinary retry. */
        if (s_last_reason == WIFI_REASON_NO_AP_FOUND && !s_scan_logged) {
            s_scan_logged = true;
            log_visible_aps();
        }

        s_backoff_sec *= 2;
        if (s_backoff_sec > CONFIG_LW_WIFI_MAX_BACKOFF_SEC) {
            s_backoff_sec = CONFIG_LW_WIFI_MAX_BACKOFF_SEC;
        }
        esp_wifi_connect();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        s_last_reason = (ev != NULL) ? ev->reason : 0;
        ESP_LOGW(TAG, "disconnected (reason %d%s)", s_last_reason,
                 s_last_reason == WIFI_REASON_NO_AP_FOUND ? " = AP not found" : "");
        s_connected = false;
        switch (s_last_reason) {
            case WIFI_REASON_NO_AP_FOUND:
                s_ui_state = WIFI_UI_NO_AP;
                break;
            case WIFI_REASON_AUTH_FAIL:
            case WIFI_REASON_AUTH_EXPIRE:
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                s_ui_state = WIFI_UI_BAD_AUTH;
                break;
            default:
                s_ui_state = WIFI_UI_OTHER_FAILURE;
                break;
        }
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        if (s_retry_task != NULL) {
            xTaskNotifyGive(s_retry_task);
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected, ip " IPSTR, IP2STR(&ev->ip_info.ip));
        /* A successful association resets the backoff, so a brief outage
         * does not leave the next reconnect waiting minutes. */
        s_backoff_sec = BACKOFF_START_SEC;
        s_scan_logged = false;
        s_connected = true;
        s_ui_state = WIFI_UI_CONNECTED;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_start(void)
{
    if (strlen(CONFIG_LW_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "no SSID configured -- see README, 'Wi-Fi 資格情報' section");
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_LW_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_LW_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));

    /* Keep the radio awake: a display that updates on a timer has no use for
     * power save, and modem sleep adds latency to every fetch. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (xTaskCreate(retry_task, "wifi_retry", 3072, NULL, 4, &s_retry_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ui_state = WIFI_UI_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "starting, ssid \"%s\"", CONFIG_LW_WIFI_SSID);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

wifi_ui_state_t wifi_ui_state(void)
{
    return s_ui_state;
}

int wifi_last_reason(void)
{
    return s_last_reason;
}

bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return false;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
