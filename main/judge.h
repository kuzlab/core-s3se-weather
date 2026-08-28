/* Laundry verdict logic (SPEC §1.2).
 *
 * Deliberately free of ESP-IDF, network and LVGL dependencies: this is the
 * one part that gets re-tuned repeatedly, so it has to be testable on the
 * host. Pure functions only, no global state.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JUDGE_MAX_SLOTS   48
#define JUDGE_HORIZON_H   24
#define JUDGE_SLOT_SEC    3600

typedef struct {
    time_t t;    /* slot start, UTC unixtime */
    float  mm;   /* precipitation over the *preceding* hour [mm] */
    int    pop;  /* probability of precipitation [%]; 0 when absent */
} hour_slot_t;

typedef enum {
    V_UNKNOWN = 0,
    V_OK,
    V_CAUTION,
    V_BRING_IN,
    V_RAINING,
} verdict_t;

typedef struct {
    verdict_t verdict;
    int       hours_to_rain;  /* -1 = no rain within the horizon */
    time_t    rain_start;
    time_t    rain_end;
    float     rain_total_mm;
    int       rain_peak_pop;
} judgement_t;

typedef struct {
    float rain_mm;     /* at or above this counts as rain [mm/h] */
    int   rain_pop;    /* at or above this counts as rain [%] */
    int   bring_in_h;  /* rain within this many hours -> V_BRING_IN */
    int   caution_h;   /* rain within this many hours -> V_CAUTION */
} judge_config_t;

/* A slot is rainy if either the amount or the probability crosses its
 * threshold. Both are noisy in different ways, so either one is enough. */
bool judge_is_rainy(const hour_slot_t *slot, const judge_config_t *cfg);

/* Scans forward from the slot containing `now` over JUDGE_HORIZON_H hours.
 * Returns a verdict of V_UNKNOWN when `now` falls outside the slot range,
 * which is what happens before the clock has been synchronised. */
judgement_t judge_evaluate(const hour_slot_t *slots, int n_slots,
                           time_t now, const judge_config_t *cfg);

/* Radar nowcast override (SPEC §1.2). Raises the verdict to at least
 * V_BRING_IN when recent observed intensity crosses the threshold, because
 * radar beats the forecast model over the next hour. A negative
 * nowcast_max_mm_h means "not fetched" and changes nothing. */
void judge_apply_nowcast(judgement_t *j, float nowcast_max_mm_h, float threshold);

const char *judge_verdict_label(verdict_t v);

#ifdef __cplusplus
}
#endif
