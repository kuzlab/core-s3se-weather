#include "app_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static app_state_t       s_state;
static SemaphoreHandle_t s_mutex;

esp_err_t app_state_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(&s_state, 0, sizeof(s_state));
    s_state.judge.verdict = V_UNKNOWN;
    s_state.judge.hours_to_rain = -1;
    s_state.nowcast_max_mm_h = -1.0f;
    return ESP_OK;
}

bool app_state_lock(uint32_t timeout_ms)
{
    if (s_mutex == NULL) {
        return false;
    }
    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void app_state_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

app_state_t *app_state_locked(void)
{
    return &s_state;
}

void app_state_get(app_state_t *out)
{
    if (out == NULL) {
        return;
    }
    if (app_state_lock(1000)) {
        memcpy(out, &s_state, sizeof(*out));
        app_state_unlock();
    } else {
        memset(out, 0, sizeof(*out));
        out->judge.verdict = V_UNKNOWN;
        out->judge.hours_to_rain = -1;
        out->nowcast_max_mm_h = -1.0f;
    }
}
