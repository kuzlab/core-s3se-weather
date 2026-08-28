/* Laundry weather display for M5Stack CoreS3 SE.
 *
 * Milestone 1: bring up the internal I2C bus and the board power sequence,
 * then report what is actually on the bus. See SPEC §9.
 */
#include <inttypes.h>

#include "cores3se_bsp.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "laundry-weather starting (%s)", CONFIG_LW_PLACE_NAME);
    ESP_LOGI(TAG, "free heap at boot: %" PRIu32 " bytes", esp_get_free_heap_size());

    init_nvs();

    esp_err_t err = bsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s -- halting", esp_err_to_name(err));
        return;
    }

    bsp_probe_panel_variant();

    /* No display driver yet, so the panel shows nothing meaningful. Turning
     * the backlight on still proves the AXP2101 DLDO1 rail works, which is
     * the part that usually fails silently. */
    ESP_ERROR_CHECK(bsp_backlight_set(CONFIG_LW_BRIGHTNESS_DAY));
    ESP_LOGI(TAG, "backlight on -- the screen should be visibly lit");

    ESP_LOGI(TAG, "milestone 1 complete");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes, min ever: %" PRIu32,
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
    }
}
