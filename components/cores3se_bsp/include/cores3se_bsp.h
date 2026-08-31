/* Board support for M5Stack CoreS3 SE.
 *
 * Wraps the AXP2101 / AW9523B / BM8563 drivers into one power-on sequence.
 * Everything here is derived from the primary sources listed in
 * docs/HARDWARE.md -- do not change register values without updating that file.
 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"
#include "esp_err.h"

#include "aw9523b.h"
#include "axp2101.h"
#include "bm8563.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal I2C bus (SPEC §4.2) */
#define BSP_I2C_PORT     I2C_NUM_0
#define BSP_I2C_SDA_GPIO 12
#define BSP_I2C_SCL_GPIO 11

/* SPI bus shared by the LCD. microSD is deliberately not used, so GPIO35 is
 * treated as a plain LCD D/C output (SPEC §4.2, docs/HARDWARE.md §2-2). */
#define BSP_LCD_SPI_HOST   SPI2_HOST
#define BSP_LCD_MOSI_GPIO  37
#define BSP_LCD_SCLK_GPIO  36
#define BSP_LCD_DC_GPIO    35
#define BSP_LCD_CS_GPIO    3

#define BSP_LCD_H_RES 320
#define BSP_LCD_V_RES 240

#define BSP_TOUCH_I2C_ADDR 0x38

/* Which ILI934x variant this unit has. Determined from the touch controller's
 * firmware ID, because the panel itself cannot be read back reliably.
 * See docs/HARDWARE.md §2-1. */
typedef enum {
    BSP_PANEL_UNKNOWN = 0,
    BSP_PANEL_ILI9342C,
    BSP_PANEL_ILI9342E,
} bsp_panel_variant_t;

/* Initialises the internal I2C bus and runs the CoreS3 SE power-on sequence
 * (AW9523B port setup, then AXP2101 rails). Safe to call once at startup. */
esp_err_t bsp_init(void);

i2c_master_bus_handle_t bsp_i2c_bus(void);
aw9523b_handle_t        bsp_aw9523b(void);
axp2101_handle_t        bsp_axp2101(void);
bm8563_handle_t         bsp_bm8563(void);

/* 0 = backlight off, 1-255 = DLDO1 2.5V..3.3V. */
esp_err_t bsp_backlight_set(uint8_t brightness);

/* Brings up the SPI bus and the ILI934x panel. bsp_init() must have run
 * first: the panel's reset line hangs off the AW9523B, and its supply rail
 * off the AXP2101. Both handles are optional outputs. */
esp_err_t bsp_display_init(esp_lcd_panel_handle_t *out_panel,
                           esp_lcd_panel_io_handle_t *out_io);

/* Fills the whole panel with one RGB565 colour. Milestone 2's check. */
esp_err_t bsp_display_fill(esp_lcd_panel_handle_t panel, uint16_t rgb565);

/* Brings up the FT6336U touch controller on the internal I2C bus.
 * The interrupt line is behind the AW9523B, so this is polled: the handle is
 * meant to be driven by LVGL's input-device read callback, not by an ISR
 * (SPEC §4.2). */
esp_err_t bsp_touch_init(esp_lcd_touch_handle_t *out_touch);

/* Speaker via the AW88298. Optional: if this fails the display is
 * unaffected and bsp_audio_available() stays false (SPEC §4.2). */
esp_err_t bsp_audio_init(void);
bool      bsp_audio_available(void);

/* Two short rising notes, played when the verdict gets worse (SPEC §1.4). */
esp_err_t bsp_beep_alert(void);

/* Stops the I2S clocks and the amplifier. Call before a software restart so
 * no peripheral is still driving pins while the chip comes back up. */
void bsp_audio_quiesce(void);

/* Reads the PMIC's rail-enable and backlight registers back and logs them,
 * repairing the rail register if it no longer matches what was written.
 * Returns true if a repair was needed.
 *
 * Worth running periodically: a blank screen with a healthy UI task means
 * the panel lost power or backlight, and nothing else in the system would
 * notice. */
bool bsp_power_health_check(void);

/* Probes every 7-bit address and logs what answered. Milestone 1's check. */
void bsp_i2c_scan_log(void);

/* Reads the touch controller's version registers to decide which panel init
 * sequence milestone 2 needs. Logs the raw values either way. */
bsp_panel_variant_t bsp_probe_panel_variant(void);

#ifdef __cplusplus
}
#endif
