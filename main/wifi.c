#include "wifi.h"

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

/* Reconnecting from the event handler would block the event loop for the
 * whole backoff, so the delay is served on its own task. */
static void retry_task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const int delay = s_backoff_sec;
        ESP_LOGI(TAG, "reconnecting in %d s", delay);
        vTaskDelay(pdMS_TO_TICKS(delay * 1000));

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
        ESP_LOGW(TAG, "disconnected (reason %d)", ev != NULL ? ev->reason : -1);
        s_connected = false;
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
        s_connected = true;
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

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "starting, ssid \"%s\"", CONFIG_LW_WIFI_SSID);
    return ESP_OK;
}

bool wifi_is_connected(void)
{
    return s_connected;
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
