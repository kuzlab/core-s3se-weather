/* Host-side tests for the verdict logic (SPEC §9 milestone 7).
 *
 * Runs on the development machine, not the device: judge.c has no ESP-IDF
 * dependencies precisely so this stays possible. Build with test/run.sh.
 */
#include "judge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failures++;                                                  \
            printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                  \
    } while (0)

static const judge_config_t DEFAULT_CFG = {
    .rain_mm    = 0.2f,
    .rain_pop   = 50,
    .bring_in_h = 2,
    .caution_h  = 5,
};

/* Slot 0 starts at an arbitrary but fixed epoch so tests are deterministic. */
#define T0 ((time_t)1756339200)

static void fill_dry(hour_slot_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        s[i].t   = T0 + (time_t)i * JUDGE_SLOT_SEC;
        s[i].mm  = 0.0f;
        s[i].pop = 0;
    }
}

static void test_all_dry_is_ok(void)
{
    printf("all dry -> V_OK\n");
    hour_slot_t s[48];
    fill_dry(s, 48);

    judgement_t j = judge_evaluate(s, 48, T0 + 100, &DEFAULT_CFG);
    CHECK(j.verdict == V_OK, "verdict=%d expected V_OK", j.verdict);
    CHECK(j.hours_to_rain == -1, "hours_to_rain=%d expected -1", j.hours_to_rain);
}

static void test_raining_now(void)
{
    printf("rain in the current slot -> V_RAINING\n");
    hour_slot_t s[48];
    fill_dry(s, 48);
    s[3].mm = 1.5f;

    judgement_t j = judge_evaluate(s, 48, s[3].t + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_RAINING, "verdict=%d expected V_RAINING", j.verdict);
    CHECK(j.hours_to_rain == 0, "hours_to_rain=%d expected 0", j.hours_to_rain);
}

static void test_verdict_boundaries(void)
{
    printf("hour-offset boundaries\n");
    /* bring_in_h=2, caution_h=5, so: 1,2 -> BRING_IN; 3,4,5 -> CAUTION;
     * 6 and beyond -> OK. The boundary values are the ones worth pinning. */
    const struct { int offset; verdict_t want; } cases[] = {
        { 1, V_BRING_IN }, { 2, V_BRING_IN },
        { 3, V_CAUTION },  { 5, V_CAUTION },
        { 6, V_OK },       { 12, V_OK },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        hour_slot_t s[48];
        fill_dry(s, 48);
        s[cases[i].offset].mm = 1.0f;

        judgement_t j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
        CHECK(j.verdict == cases[i].want,
              "offset=%d verdict=%d expected %d",
              cases[i].offset, j.verdict, cases[i].want);
        CHECK(j.hours_to_rain == cases[i].offset,
              "offset=%d hours_to_rain=%d", cases[i].offset, j.hours_to_rain);
    }
}

static void test_probability_alone_triggers_rain(void)
{
    printf("probability alone counts as rain\n");
    hour_slot_t s[48];
    fill_dry(s, 48);
    s[1].mm  = 0.0f;
    s[1].pop = 50;   /* exactly at the threshold */

    judgement_t j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_BRING_IN, "verdict=%d expected V_BRING_IN", j.verdict);

    /* One below the threshold must not trigger. */
    s[1].pop = 49;
    j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_OK, "pop=49 verdict=%d expected V_OK", j.verdict);
}

static void test_amount_threshold_is_inclusive(void)
{
    printf("amount threshold is inclusive\n");
    hour_slot_t s[48];
    fill_dry(s, 48);

    s[1].mm = 0.2f;
    judgement_t j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_BRING_IN, "mm=0.2 verdict=%d expected V_BRING_IN", j.verdict);

    s[1].mm = 0.1f;
    j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_OK, "mm=0.1 verdict=%d expected V_OK", j.verdict);
}

static void test_rain_block_extent(void)
{
    printf("rain block start/end/total/peak\n");
    hour_slot_t s[48];
    fill_dry(s, 48);
    /* A three-hour block at offsets 4,5,6 with a dry slot after it. */
    s[4].mm = 0.5f; s[4].pop = 60;
    s[5].mm = 1.4f; s[5].pop = 80;
    s[6].mm = 0.5f; s[6].pop = 70;

    judgement_t j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.hours_to_rain == 4, "hours_to_rain=%d expected 4", j.hours_to_rain);
    CHECK(j.rain_start == s[4].t, "rain_start mismatch");
    CHECK(j.rain_end == s[6].t + JUDGE_SLOT_SEC, "rain_end mismatch");
    CHECK(j.rain_total_mm > 2.39f && j.rain_total_mm < 2.41f,
          "rain_total_mm=%.3f expected 2.4", (double)j.rain_total_mm);
    CHECK(j.rain_peak_pop == 80, "rain_peak_pop=%d expected 80", j.rain_peak_pop);
}

static void test_horizon_is_24h(void)
{
    printf("rain beyond 24h is out of horizon\n");
    hour_slot_t s[48];
    fill_dry(s, 48);
    s[24].mm = 5.0f;  /* exactly one hour past the last scanned slot */

    judgement_t j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_OK, "verdict=%d expected V_OK", j.verdict);
    CHECK(j.hours_to_rain == -1, "hours_to_rain=%d expected -1", j.hours_to_rain);

    /* One hour earlier it is inside the horizon and must be seen. */
    fill_dry(s, 48);
    s[23].mm = 5.0f;
    j = judge_evaluate(s, 48, T0 + 60, &DEFAULT_CFG);
    CHECK(j.hours_to_rain == 23, "hours_to_rain=%d expected 23", j.hours_to_rain);
}

static void test_unknown_when_clock_outside_range(void)
{
    printf("clock outside slot range -> V_UNKNOWN\n");
    hour_slot_t s[48];
    fill_dry(s, 48);

    judgement_t before = judge_evaluate(s, 48, T0 - 1, &DEFAULT_CFG);
    CHECK(before.verdict == V_UNKNOWN, "verdict=%d expected V_UNKNOWN", before.verdict);

    judgement_t after = judge_evaluate(s, 48, s[47].t + JUDGE_SLOT_SEC, &DEFAULT_CFG);
    CHECK(after.verdict == V_UNKNOWN, "verdict=%d expected V_UNKNOWN", after.verdict);
}

static void test_empty_and_null_inputs(void)
{
    printf("degenerate inputs\n");
    hour_slot_t s[1];
    fill_dry(s, 1);

    CHECK(judge_evaluate(NULL, 48, T0, &DEFAULT_CFG).verdict == V_UNKNOWN, "null slots");
    CHECK(judge_evaluate(s, 0, T0, &DEFAULT_CFG).verdict == V_UNKNOWN, "zero slots");
    CHECK(judge_evaluate(s, 1, T0, NULL).verdict == V_UNKNOWN, "null config");
}

static void test_partial_forecast_shorter_than_horizon(void)
{
    printf("forecast shorter than the horizon\n");
    /* Only 5 slots available; scanning must stop at the end of the array
     * rather than reading past it. */
    hour_slot_t s[5];
    fill_dry(s, 5);

    judgement_t j = judge_evaluate(s, 5, T0 + 60, &DEFAULT_CFG);
    CHECK(j.verdict == V_OK, "verdict=%d expected V_OK", j.verdict);
    CHECK(j.hours_to_rain == -1, "hours_to_rain=%d expected -1", j.hours_to_rain);
}

static void test_rain_block_runs_to_end_of_data(void)
{
    printf("rain block truncated by end of data\n");
    hour_slot_t s[6];
    fill_dry(s, 6);
    s[4].mm = 1.0f;
    s[5].mm = 1.0f;   /* still raining when the data runs out */

    judgement_t j = judge_evaluate(s, 6, T0 + 60, &DEFAULT_CFG);
    CHECK(j.hours_to_rain == 4, "hours_to_rain=%d expected 4", j.hours_to_rain);
    CHECK(j.rain_end == s[5].t + JUDGE_SLOT_SEC, "rain_end mismatch");
}

static void test_nowcast_override(void)
{
    printf("nowcast override\n");
    const float threshold = 0.5f;

    judgement_t j = { .verdict = V_OK, .hours_to_rain = -1 };
    judge_apply_nowcast(&j, 0.8f, threshold);
    CHECK(j.verdict == V_BRING_IN, "V_OK + radar -> %d expected V_BRING_IN", j.verdict);

    j.verdict = V_CAUTION;
    judge_apply_nowcast(&j, 0.5f, threshold);
    CHECK(j.verdict == V_BRING_IN, "at threshold -> %d expected V_BRING_IN", j.verdict);

    /* Below the threshold changes nothing. */
    j.verdict = V_OK;
    judge_apply_nowcast(&j, 0.4f, threshold);
    CHECK(j.verdict == V_OK, "below threshold -> %d expected V_OK", j.verdict);

    /* It raises, never lowers: V_RAINING must survive. */
    j.verdict = V_RAINING;
    judge_apply_nowcast(&j, 9.0f, threshold);
    CHECK(j.verdict == V_RAINING, "V_RAINING -> %d expected unchanged", j.verdict);

    /* Not fetched (-1) is not the same as zero rainfall. */
    j.verdict = V_OK;
    judge_apply_nowcast(&j, -1.0f, threshold);
    CHECK(j.verdict == V_OK, "unfetched -> %d expected V_OK", j.verdict);

    /* An unknown verdict must stay unknown; there is no clock to trust. */
    j.verdict = V_UNKNOWN;
    judge_apply_nowcast(&j, 9.0f, threshold);
    CHECK(j.verdict == V_UNKNOWN, "V_UNKNOWN -> %d expected unchanged", j.verdict);
}

/* ---- outlook selection and windows ---------------------------------- */

static const outlook_config_t OUTLOOK_CFG = {
    .morning_start_hour = 5,
    .day_start_hour     = 12,
    .night_start_hour   = 20,
    .dry_start_hour     = 9,
    .dry_end_hour       = 17,
};

/* Builds a local (JST) timestamp; main() pins TZ so this is deterministic. */
static time_t jst(int year, int mon, int day, int hour, int min)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon  = mon - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min  = min;
    t.tm_isdst = -1;
    return mktime(&t);
}

static void test_outlook_by_hour(void)
{
    printf("outlook selection by hour\n");
    const struct { int hour; outlook_t want; } cases[] = {
        {  0, OUTLOOK_TOMORROW }, {  4, OUTLOOK_TOMORROW },
        {  5, OUTLOOK_TODAY },    { 11, OUTLOOK_TODAY },
        { 12, OUTLOOK_NOW },      { 19, OUTLOOK_NOW },
        { 20, OUTLOOK_TOMORROW }, { 23, OUTLOOK_TOMORROW },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const outlook_t got = judge_outlook(cases[i].hour, &OUTLOOK_CFG);
        CHECK(got == cases[i].want, "hour %d -> %d, expected %d",
              cases[i].hour, got, cases[i].want);
    }
}

static void test_outlook_windows(void)
{
    printf("outlook windows\n");

    /* Morning: from now until the laundry is expected to be in. */
    const time_t morning = jst(2026, 8, 29, 10, 19);
    time_t from = 0, to = 0;
    judge_outlook_window(OUTLOOK_TODAY, morning, &OUTLOOK_CFG, &from, &to);
    CHECK(from == morning, "morning window should start now");
    CHECK(to == jst(2026, 8, 29, 17, 0), "morning window should end at 17:00 today");

    /* Late evening: the next daylight is tomorrow's. */
    const time_t evening = jst(2026, 8, 29, 22, 30);
    judge_outlook_window(OUTLOOK_TOMORROW, evening, &OUTLOOK_CFG, &from, &to);
    CHECK(from == jst(2026, 8, 30, 9, 0), "22:30 -> tomorrow 09:00");
    CHECK(to == jst(2026, 8, 30, 17, 0), "22:30 -> tomorrow 17:00");

    /* Small hours: the next daylight is later the same calendar day, not
     * the one after. Getting this wrong would show the wrong day's weather
     * to anyone awake at 2am. */
    const time_t small_hours = jst(2026, 8, 30, 2, 0);
    judge_outlook_window(OUTLOOK_TOMORROW, small_hours, &OUTLOOK_CFG, &from, &to);
    CHECK(from == jst(2026, 8, 30, 9, 0), "02:00 -> same day 09:00");
    CHECK(to == jst(2026, 8, 30, 17, 0), "02:00 -> same day 17:00");
}

static void test_window_judgement(void)
{
    printf("window judgement\n");

    hour_slot_t s[48];
    const time_t base = jst(2026, 8, 29, 0, 0);
    for (int i = 0; i < 48; i++) {
        s[i].t   = base + (time_t)i * JUDGE_SLOT_SEC;
        s[i].mm  = 0.0f;
        s[i].pop = 0;
    }

    const time_t from = jst(2026, 8, 29, 9, 0);
    const time_t to   = jst(2026, 8, 29, 17, 0);   /* 8 hours */

    /* Dry all the way through. */
    window_judgement_t w = judge_window(s, 48, from, to, &DEFAULT_CFG);
    CHECK(w.verdict == V_OK, "dry window -> %d, expected V_OK", w.verdict);
    CHECK(w.window_hours == 8, "window_hours=%d expected 8", w.window_hours);
    CHECK(!w.has_rain, "dry window should report no rain");

    /* One wet hour late in the window is a caution, not a washout. */
    s[15].mm = 1.0f;   /* 15:00 */
    w = judge_window(s, 48, from, to, &DEFAULT_CFG);
    CHECK(w.verdict == V_CAUTION, "one wet hour -> %d, expected V_CAUTION", w.verdict);
    CHECK(w.rainy_hours == 1, "rainy_hours=%d expected 1", w.rainy_hours);
    CHECK(w.rain_start == s[15].t, "rain_start mismatch");

    /* A third of the window wet is a washout. */
    s[13].mm = 1.0f;
    s[14].mm = 1.0f;
    w = judge_window(s, 48, from, to, &DEFAULT_CFG);
    CHECK(w.verdict == V_BRING_IN, "3 of 8 wet -> %d, expected V_BRING_IN", w.verdict);

    /* Rain at the very start leaves no dry stretch to work with. */
    for (int i = 0; i < 48; i++) {
        s[i].mm = 0.0f;
    }
    s[9].mm = 1.0f;   /* 09:00, the first slot of the window */
    w = judge_window(s, 48, from, to, &DEFAULT_CFG);
    CHECK(w.verdict == V_RAINING, "rain at window start -> %d, expected V_RAINING", w.verdict);

    /* A window the forecast does not reach must not produce a verdict. */
    w = judge_window(s, 48, base + 100 * JUDGE_SLOT_SEC,
                     base + 108 * JUDGE_SLOT_SEC, &DEFAULT_CFG);
    CHECK(w.verdict == V_UNKNOWN, "window beyond the forecast -> %d, expected V_UNKNOWN",
          w.verdict);
    CHECK(w.window_hours == 0, "window_hours=%d expected 0", w.window_hours);
}

int main(void)
{
    /* The window maths is local-time based, so pin the zone the device uses
     * rather than inheriting whatever the build machine has. */
    setenv("TZ", "JST-9", 1);
    tzset();

    printf("test_judge\n\n");

    test_all_dry_is_ok();
    test_raining_now();
    test_verdict_boundaries();
    test_probability_alone_triggers_rain();
    test_amount_threshold_is_inclusive();
    test_rain_block_extent();
    test_horizon_is_24h();
    test_unknown_when_clock_outside_range();
    test_empty_and_null_inputs();
    test_partial_forecast_shorter_than_horizon();
    test_rain_block_runs_to_end_of_data();
    test_nowcast_override();
    test_outlook_by_hour();
    test_outlook_windows();
    test_window_judgement();

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
