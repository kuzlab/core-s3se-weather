/* AXP2101 PMIC driver (CoreS3 SE internal I2C).
 *
 * Only the rails CoreS3 SE actually needs are exposed. Register values come
 * from m5stack/M5GFX; see docs/HARDWARE.md §1-2 and §1-3.
 *
 * NOTE: CoreS3 SE has no built-in battery (SPEC §4.3), so there is
 * deliberately no battery-gauge API here.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AXP2101_I2C_ADDR 0x34

#define AXP2101_REG_LDO_EN0   0x90  /* LDOS on/off control 0 */
#define AXP2101_REG_ALDO3_VOL 0x94
#define AXP2101_REG_ALDO4_VOL 0x95
#define AXP2101_REG_DLDO1_VOL 0x99  /* LCD backlight brightness */

#define AXP2101_LDO_EN0_DLDO1 (1 << 7)

/* Voltage register encoding for the *LDO rails: V = 0.5V + 0.1V * N */
#define AXP2101_LDO_VOLT_REG(mv) ((uint8_t)(((mv) - 500) / 100))

typedef struct axp2101_t *axp2101_handle_t;

esp_err_t axp2101_create(i2c_master_bus_handle_t bus, axp2101_handle_t *out);
void      axp2101_destroy(axp2101_handle_t h);

esp_err_t axp2101_read(axp2101_handle_t h, uint8_t reg, uint8_t *val);
esp_err_t axp2101_write(axp2101_handle_t h, uint8_t reg, uint8_t val);
esp_err_t axp2101_set_bits(axp2101_handle_t h, uint8_t reg, uint8_t bits);
esp_err_t axp2101_clear_bits(axp2101_handle_t h, uint8_t reg, uint8_t bits);

/* Brings up the rails CoreS3 SE needs (ALDO1-4, BLDO1-2, DLDO1). */
esp_err_t axp2101_cores3se_power_on(axp2101_handle_t h);

/* LCD backlight. brightness 0 = off, 1-255 maps onto DLDO1 = 2.5V..3.3V.
 * That 9-step range is the entire usable span; see docs/HARDWARE.md §1-3. */
esp_err_t axp2101_set_backlight(axp2101_handle_t h, uint8_t brightness);

#ifdef __cplusplus
}
#endif
