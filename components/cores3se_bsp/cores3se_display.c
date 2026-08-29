/* LCD bring-up for CoreS3 SE.
 *
 * The panel is an ILI9342C (some units are ILI9342E; see docs/HARDWARE.md
 * §2-1), which the esp_lcd_ili9341 driver handles. Reset and backlight are
 * not GPIOs -- they hang off the AW9523B and the AXP2101 respectively, so
 * bsp_init() has to run before this.
 */
#include <stdlib.h>
#include <string.h>

#include "cores3se_bsp.h"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"

static const char *TAG = "bsp.lcd";

#define LCD_PIXEL_CLOCK_HZ  (40 * 1000 * 1000)  /* M5GFX writes at 40MHz */
#define LCD_CMD_BITS        8
#define LCD_PARAM_BITS      8

/* One full row of RGB565 pixels is the unit bsp_display_fill() pushes. */
#define LCD_FILL_ROWS       16

/* The SPI bus has to be sized for the largest single transfer anyone will
 * make over it. That is an LVGL flush, not the small strips
 * bsp_display_fill() uses: sizing it to the strip made esp_lcd split every
 * flush into chunked transactions, multiplying the chances of losing a
 * completion, and LVGL waits for that completion in an untimed busy loop
 * while holding its lock. 48 lines leaves headroom over the 40-line LVGL
 * draw buffer without reserving DMA descriptors for a whole frame. */
#define LCD_MAX_TRANSFER_LINES 48
#define LCD_MAX_TRANSFER_SZ (BSP_LCD_H_RES * LCD_MAX_TRANSFER_LINES * (int)sizeof(uint16_t))

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;

esp_err_t bsp_display_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_panel_io_handle_t *out_io)
{
    if (s_panel != NULL) {
        if (out_panel) { *out_panel = s_panel; }
        if (out_io)    { *out_io = s_io; }
        return ESP_OK;
    }

    /* MISO is left unconnected on purpose: GPIO35 is shared between the SD
     * card's MISO and the LCD's D/C, and this project does not use the SD
     * card, so GPIO35 belongs to the panel IO as D/C (docs/HARDWARE.md §2-2). */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = BSP_LCD_MOSI_GPIO,
        .miso_io_num     = GPIO_NUM_NC,
        .sclk_io_num     = BSP_LCD_SCLK_GPIO,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = BSP_LCD_DC_GPIO,
        .cs_gpio_num       = BSP_LCD_CS_GPIO,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_HOST,
                                                 &io_cfg, &s_io),
                        TAG, "panel IO init failed");

    /* reset_gpio_num is NC because the reset line is on the I/O expander;
     * bsp_init() has already pulsed it. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9341(s_io, &panel_cfg, &s_panel),
                        TAG, "panel init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");

    /* The CoreS3 panel is wired inverted; without this everything is a
     * photographic negative. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, false), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on failed");

    ESP_LOGI(TAG, "panel up: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);

    if (out_panel) { *out_panel = s_panel; }
    if (out_io)    { *out_io = s_io; }
    return ESP_OK;
}

esp_err_t bsp_display_fill(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    if (panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t px = (size_t)BSP_LCD_H_RES * LCD_FILL_ROWS;
    uint16_t *row = heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (row == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* The panel takes RGB565 big-endian, so swap here rather than relying on
     * a driver flag. This is the same byte order LVGL will need later. */
    const uint16_t be = (uint16_t)((rgb565 >> 8) | (rgb565 << 8));
    for (size_t i = 0; i < px; i++) {
        row[i] = be;
    }

    esp_err_t err = ESP_OK;
    for (int y = 0; y < BSP_LCD_V_RES && err == ESP_OK; y += LCD_FILL_ROWS) {
        const int y_end = (y + LCD_FILL_ROWS > BSP_LCD_V_RES) ? BSP_LCD_V_RES : y + LCD_FILL_ROWS;
        err = esp_lcd_panel_draw_bitmap(panel, 0, y, BSP_LCD_H_RES, y_end, row);
    }

    free(row);
    return err;
}
