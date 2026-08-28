/* AW9523B I/O expander driver (CoreS3 SE internal I2C).
 *
 * Register values and CoreS3 SE port assignments are documented in
 * docs/HARDWARE.md §1-1, derived from m5stack/M5GFX.
 */
#pragma once

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AW9523B_I2C_ADDR 0x58

#define AW9523B_REG_INPUT_P0   0x00
#define AW9523B_REG_INPUT_P1   0x01
#define AW9523B_REG_OUTPUT_P0  0x02
#define AW9523B_REG_OUTPUT_P1  0x03
#define AW9523B_REG_CONFIG_P0  0x04  /* 1 = input, 0 = output */
#define AW9523B_REG_CONFIG_P1  0x05
#define AW9523B_REG_ID         0x10  /* reads back AW9523B_ID_VALUE */
#define AW9523B_REG_GCR        0x11
#define AW9523B_REG_LEDMODE_P0 0x12  /* 1 = GPIO mode, 0 = LED current mode */
#define AW9523B_REG_LEDMODE_P1 0x13

#define AW9523B_ID_VALUE       0x23

/* CoreS3 SE port assignments */
#define AW9523B_P0_TOUCH_RST   (1 << 0)  /* port 0 */
#define AW9523B_P0_BOOST_EN    (1 << 1)
#define AW9523B_P0_BUS_OUT_EN  (1 << 2)
#define AW9523B_P1_LCD_RST     (1 << 1)  /* port 1 */
#define AW9523B_P1_TOUCH_INT   (1 << 2)  /* input */

typedef struct aw9523b_t *aw9523b_handle_t;

esp_err_t aw9523b_create(i2c_master_bus_handle_t bus, aw9523b_handle_t *out);
void      aw9523b_destroy(aw9523b_handle_t h);

esp_err_t aw9523b_read(aw9523b_handle_t h, uint8_t reg, uint8_t *val);
esp_err_t aw9523b_write(aw9523b_handle_t h, uint8_t reg, uint8_t val);
esp_err_t aw9523b_set_bits(aw9523b_handle_t h, uint8_t reg, uint8_t bits);
esp_err_t aw9523b_clear_bits(aw9523b_handle_t h, uint8_t reg, uint8_t bits);

/* Drives LCD_RST (port 1, P1_1). level=true releases reset. */
esp_err_t aw9523b_lcd_reset_level(aw9523b_handle_t h, bool level);
/* Drives TOUCH_RST (port 0, P0_0). level=true releases reset. */
esp_err_t aw9523b_touch_reset_level(aw9523b_handle_t h, bool level);

#ifdef __cplusplus
}
#endif
