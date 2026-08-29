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

outlook_t judge_outlook(int local_hour, const outlook_config_t *oc)
{
    if (oc == NULL) {
        return OUTLOOK_NOW;
    }
    if (local_hour >= oc->night_start_hour || local_hour < oc->morning_start_hour) {
        return OUTLOOK_TOMORROW;
    }
    if (local_hour < oc->day_start_hour) {
        return OUTLOOK_TODAY;
    }
    return OUTLOOK_NOW;
}

/* Local midnight of the day containing t, as unixtime. */
static time_t local_midnight(time_t t)
{
    struct tm lt;
    localtime_r(&t, &lt);
    lt.tm_hour = 0;
    lt.tm_min  = 0;
    lt.tm_sec  = 0;
    lt.tm_isdst = -1;
    return mktime(&lt);
}

void judge_outlook_window(outlook_t outlook, time_t now,
                          const outlook_config_t *oc,
                          time_t *from, time_t *to)
{
    if (from == NULL || to == NULL || oc == NULL) {
        return;
    }
    *from = 0;
    *to = 0;

    struct tm lt;
    localtime_r(&now, &lt);
    const time_t midnight = local_midnight(now);

    switch (outlook) {
        case OUTLOOK_TODAY:
            /* From right now, because the question is whether a load hung
             * now survives the day -- not what the whole day looks like on
             * average. Ends when the laundry is expected to come in. */
            *from = now;
            *to   = midnight + (time_t)oc->dry_end_hour * JUDGE_SLOT_SEC;
            break;

        case OUTLOOK_TOMORROW: {
            /* The next daylight stretch that has not happened yet. After
             * 20:00 that is tomorrow; in the small hours it is later the
             * same calendar day. */
            const int day_offset = (lt.tm_hour >= oc->night_start_hour) ? 1 : 0;
            const time_t base = midnight + (time_t)day_offset * 24 * JUDGE_SLOT_SEC;
            *from = base + (time_t)oc->dry_start_hour * JUDGE_SLOT_SEC;
            *to   = base + (time_t)oc->dry_end_hour * JUDGE_SLOT_SEC;
            break;
        }

        case OUTLOOK_NOW:
        default:
            /* Answered by judge_evaluate(), not by a window. */
            break;
    }

    if (*to < *from) {
        *to = *from;
    }
}

window_judgement_t judge_window(const hour_slot_t *slots, int n_slots,
                                time_t from, time_t to,
                                const judge_config_t *cfg)
{
    window_judgement_t out;
    memset(&out, 0, sizeof(out));
    out.verdict = V_UNKNOWN;

    if (slots == NULL || cfg == NULL || n_slots <= 0 || to <= from) {
        return out;
    }

    int first_rainy = -1;
    int last_rainy = -1;

    for (int i = 0; i < n_slots; i++) {
        /* A slot counts when its hour overlaps the window at all. */
        const time_t slot_end = slots[i].t + JUDGE_SLOT_SEC;
        if (slot_end <= from || slots[i].t >= to) {
            continue;
        }
        out.window_hours++;

        if (!judge_is_rainy(&slots[i], cfg)) {
            continue;
        }
        out.rainy_hours++;
        out.total_mm += slots[i].mm;
        if (slots[i].pop > out.peak_pop) {
            out.peak_pop = slots[i].pop;
        }
        if (first_rainy < 0) {
            first_rainy = i;
        }
        last_rainy = i;
    }

    if (out.window_hours == 0) {
        /* The forecast does not reach this window yet. */
        return out;
    }

    if (out.rainy_hours == 0) {
        out.verdict = V_OK;
        return out;
    }

    out.has_rain = true;
    out.rain_start = slots[first_rainy].t;
    out.rain_end   = slots[last_rainy].t + JUDGE_SLOT_SEC;

    /* Rain at the very start of the window is the strongest signal: there is
     * no dry stretch to get ahead of. Otherwise grade by how much of the
     * window is wet, so one marginal hour late in the day does not read the
     * same as a washed-out afternoon. */
    if (slots[first_rainy].t <= from) {
        out.verdict = V_RAINING;
    } else if (out.rainy_hours * 3 >= out.window_hours) {
        out.verdict = V_BRING_IN;
    } else {
        out.verdict = V_CAUTION;
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
