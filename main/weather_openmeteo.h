/* Open-Meteo hourly forecast fetch and parse (SPEC §2.1).
 *
 * Data: Open-Meteo (https://open-meteo.com/), licensed CC BY 4.0.
 */
#pragma once

#include "esp_err.h"
#include "judge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hour_slot_t slots[JUDGE_MAX_SLOTS];
    int         n_slots;
} openmeteo_result_t;

/* Fetches and parses the forecast. On any failure the caller must keep
 * showing the previous data rather than clearing the screen (SPEC §8.1). */
esp_err_t openmeteo_fetch(openmeteo_result_t *out);

#ifdef __cplusplus
}
#endif
