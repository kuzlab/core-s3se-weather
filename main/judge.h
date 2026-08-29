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

/* What the user is actually asking at this time of day. The same forecast
 * answers a different question in the morning than it does at dusk: being
 * told to bring the laundry in at 10am, before any has been hung, is noise.
 */
typedef enum {
    OUTLOOK_TODAY = 0,  /* morning: can I hang a load today? */
    OUTLOOK_NOW,        /* daytime: should I bring in what is hanging? */
    OUTLOOK_TOMORROW,   /* evening and night: will the next daylight work? */
} outlook_t;

typedef struct {
    int morning_start_hour;  /* OUTLOOK_TODAY begins */
    int day_start_hour;      /* OUTLOOK_NOW begins */
    int night_start_hour;    /* OUTLOOK_TOMORROW begins */
    int dry_start_hour;      /* earliest useful drying hour */
    int dry_end_hour;        /* laundry is expected to be in by now */
} outlook_config_t;

outlook_t judge_outlook(int local_hour, const outlook_config_t *oc);

/* The half-open [from, to) span the outlook is about, as UTC unixtime.
 * OUTLOOK_NOW returns an empty span: it is answered by judge_evaluate()
 * scanning forward from now, not by looking at a fixed window. */
void judge_outlook_window(outlook_t outlook, time_t now,
                          const outlook_config_t *oc,
                          time_t *from, time_t *to);

typedef struct {
    verdict_t verdict;
    bool      has_rain;
    time_t    rain_start;
    time_t    rain_end;
    float     total_mm;
    int       peak_pop;
    int       rainy_hours;
    int       window_hours;   /* slots actually covered by the forecast */
} window_judgement_t;

/* Judges a fixed span rather than "time until the next rain". Used for the
 * planning outlooks, where the question is whether a whole stretch of the
 * day stays dry. */
window_judgement_t judge_window(const hour_slot_t *slots, int n_slots,
                                time_t from, time_t to,
                                const judge_config_t *cfg);

/* Radar nowcast override (SPEC §1.2). Raises the verdict to at least
 * V_BRING_IN when recent observed intensity crosses the threshold, because
 * radar beats the forecast model over the next hour. A negative
 * nowcast_max_mm_h means "not fetched" and changes nothing. */
void judge_apply_nowcast(judgement_t *j, float nowcast_max_mm_h, float threshold);

const char *judge_verdict_label(verdict_t v);

#ifdef __cplusplus
}
#endif
