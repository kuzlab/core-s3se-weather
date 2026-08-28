/* FT6336U touch bring-up for CoreS3 SE.
 *
 * The controller's INT line goes to the AW9523B rather than to a GPIO the
 * ESP32-S3 can take an edge interrupt on, so this is deliberately polled
 * (SPEC §4.2). LVGL's input device polls by default, so nothing is lost.
 * Reset is also on the expander and bsp_init() has already released it.
 */
#include "cores3se_bsp.h"

#include "esp_check.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

static const char *TAG = "bsp.touch";

static esp_lcd_touch_handle_t s_touch;

esp_err_t bsp_touch_init(esp_lcd_touch_handle_t *out_touch)
{
    if (s_touch != NULL) {
        if (out_touch) { *out_touch = s_touch; }
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_bus();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG, "bsp_init() must run first");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), TAG, "touch IO failed");

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        /* Both lines live on the AW9523B, so esp_lcd_touch must not try to
         * drive them as GPIOs. */
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy  = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(io, &touch_cfg, &s_touch),
                        TAG, "touch init failed");

    ESP_LOGI(TAG, "FT6336U up (polled, %dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (out_touch) { *out_touch = s_touch; }
    return ESP_OK;
}
