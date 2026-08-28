/* BM8563 RTC driver (CoreS3 SE internal I2C). PCF8563-compatible.
 *
 * Used to keep wall-clock time across power cycles so the UI can show a
 * plausible time before Wi-Fi/SNTP come up (SPEC §8.2).
 */
#pragma once

#include <time.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BM8563_I2C_ADDR 0x51

typedef struct bm8563_t *bm8563_handle_t;

esp_err_t bm8563_create(i2c_master_bus_handle_t bus, bm8563_handle_t *out);
void      bm8563_destroy(bm8563_handle_t h);

/* Reads the RTC into *out as UTC. Returns ESP_ERR_INVALID_STATE when the
 * chip's voltage-low flag is set, i.e. the held time is not trustworthy. */
esp_err_t bm8563_get_time(bm8563_handle_t h, struct tm *out);

/* Writes UTC time to the RTC and clears the voltage-low flag. */
esp_err_t bm8563_set_time(bm8563_handle_t h, const struct tm *t);

#ifdef __cplusplus
}
#endif
