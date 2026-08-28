/* Laundry weather display for M5Stack CoreS3 SE.
 *
 * Milestones 1-2: bring up the board (I2C, PMIC rails, backlight) and the
 * LCD, then prove both with a visible fill and a device inventory.
 *
 * The diagnostic block is reprinted on a timer rather than only at boot.
 * This board accepts no software reset -- not esptool's RTS reset, not the
 * RST button -- so the only way to restart it is to unplug USB. Printing
 * once at boot would mean racing the log capture against that replug.
 */
#include <inttypes.h>

#include "cores3se_bsp.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define DIAG_INTERVAL_MS 20000

/* RGB565 cycle for the fill test. Primaries make a wrong colour order
 * obvious: if red and blue swap, the byte order or rgb_ele_order is wrong. */
static const struct { uint16_t rgb565; const char *name; } FILL_COLORS[] = {
    { 0xF800, "red"   },
    { 0x07E0, "green" },
    { 0x001F, "blue"  },
    { 0xFFFF, "white" },
    { 0x0000, "black" },
};

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

    init_nvs();

    esp_err_t err = bsp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "board init failed: %s -- halting", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_handle_t panel = NULL;
    err = bsp_display_init(&panel, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        panel = NULL;
    }

    ESP_ERROR_CHECK(bsp_backlight_set(CONFIG_LW_BRIGHTNESS_DAY));
    ESP_LOGI(TAG, "backlight on at brightness %d", CONFIG_LW_BRIGHTNESS_DAY);

    size_t color_idx = 0;
    while (true) {
        ESP_LOGI(TAG, "===== diagnostics =====");
        bsp_i2c_scan_log();
        bsp_probe_panel_variant();

        if (panel != NULL) {
            const uint16_t rgb = FILL_COLORS[color_idx].rgb565;
            err = bsp_display_fill(panel, rgb);
            ESP_LOGI(TAG, "screen filled %s (0x%04x): %s",
                     FILL_COLORS[color_idx].name, rgb, esp_err_to_name(err));
            color_idx = (color_idx + 1) % (sizeof(FILL_COLORS) / sizeof(FILL_COLORS[0]));
        }

        ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes, min ever: %" PRIu32,
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());
        ESP_LOGI(TAG, "uptime: %" PRId64 " s", esp_timer_get_time() / 1000000);

        vTaskDelay(pdMS_TO_TICKS(DIAG_INTERVAL_MS));
    }
}
