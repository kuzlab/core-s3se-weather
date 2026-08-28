/* Host-side tests for the verdict logic (SPEC §9 milestone 7).
 *
 * Runs on the development machine, not the device: judge.c has no ESP-IDF
 * dependencies precisely so this stays possible. Build with test/run.sh.
 */
#include "judge.h"

#include <stdio.h>
#include <string.h>

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

int main(void)
{
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

    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
