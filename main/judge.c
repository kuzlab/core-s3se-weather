#include "judge.h"

#include <string.h>

bool judge_is_rainy(const hour_slot_t *slot, const judge_config_t *cfg)
{
    if (slot == NULL || cfg == NULL) {
        return false;
    }
    return (slot->mm >= cfg->rain_mm) || (slot->pop >= cfg->rain_pop);
}

/* Index of the slot covering `now`, or -1. Slots are assumed to be in
 * ascending order one hour apart, which is what Open-Meteo returns. */
static int judge_current_slot(const hour_slot_t *slots, int n_slots, time_t now)
{
    for (int i = 0; i < n_slots; i++) {
        if (now >= slots[i].t && now < slots[i].t + JUDGE_SLOT_SEC) {
            return i;
        }
    }
    return -1;
}

judgement_t judge_evaluate(const hour_slot_t *slots, int n_slots,
                           time_t now, const judge_config_t *cfg)
{
    judgement_t out;
    memset(&out, 0, sizeof(out));
    out.verdict = V_UNKNOWN;
    out.hours_to_rain = -1;

    if (slots == NULL || cfg == NULL || n_slots <= 0) {
        return out;
    }

    const int start = judge_current_slot(slots, n_slots, now);
    if (start < 0) {
        /* Either the clock is not set yet or the forecast has gone stale.
         * Both cases must not produce a confident-looking verdict. */
        return out;
    }

    int hit = -1;
    for (int h = 0; h < JUDGE_HORIZON_H && start + h < n_slots; h++) {
        if (judge_is_rainy(&slots[start + h], cfg)) {
            hit = h;
            break;
        }
    }

    if (hit < 0) {
        out.verdict = V_OK;
        out.hours_to_rain = -1;
        return out;
    }

    out.hours_to_rain = hit;

    /* Measure the contiguous rainy block so the summary line can say when it
     * starts, when it ends and how much falls. */
    const int first = start + hit;
    int last = first;
    out.rain_total_mm = 0.0f;
    out.rain_peak_pop = 0;
    for (int i = first; i < n_slots && judge_is_rainy(&slots[i], cfg); i++) {
        out.rain_total_mm += slots[i].mm;
        if (slots[i].pop > out.rain_peak_pop) {
            out.rain_peak_pop = slots[i].pop;
        }
        last = i;
    }
    out.rain_start = slots[first].t;
    out.rain_end   = slots[last].t + JUDGE_SLOT_SEC;

    if (hit == 0) {
        out.verdict = V_RAINING;
    } else if (hit <= cfg->bring_in_h) {
        out.verdict = V_BRING_IN;
    } else if (hit <= cfg->caution_h) {
        out.verdict = V_CAUTION;
    } else {
        out.verdict = V_OK;
    }

    return out;
}

void judge_apply_nowcast(judgement_t *j, float nowcast_max_mm_h, float threshold)
{
    if (j == NULL || nowcast_max_mm_h < 0.0f) {
        return;
    }
    if (j->verdict == V_UNKNOWN) {
        /* Without a valid clock there is no verdict to raise. */
        return;
    }
    if (nowcast_max_mm_h >= threshold && j->verdict < V_BRING_IN) {
        j->verdict = V_BRING_IN;
    }
}

const char *judge_verdict_label(verdict_t v)
{
    switch (v) {
        case V_OK:       return "干してOK";
        case V_CAUTION:  return "短時間なら可";
        case V_BRING_IN: return "まもなく取り込め";
        case V_RAINING:  return "取り込め";
        default:         return "---";
    }
}
