/* Shared state between the network task and the UI task (SPEC §3.2, §3.3).
 *
 * The network task writes; the UI task reads. Everything goes through the
 * mutex -- there is no lock-free fast path, because the struct is small and
 * updates happen at most a few times a minute.
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "judge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    hour_slot_t slots[JUDGE_MAX_SLOTS];
    int         n_slots;
    judgement_t judge;
    float       nowcast_max_mm_h;  /* < 0 when not fetched */
    time_t      last_update;       /* last *successful* fetch */
    bool        last_fetch_ok;     /* result of the most recent attempt */
} app_state_t;

esp_err_t app_state_init(void);

/* Takes a snapshot under the lock. Preferred over holding the lock across
 * rendering, which would let a slow redraw stall the network task. */
void app_state_get(app_state_t *out);

/* For the writer. Returns false on timeout; do not touch the state then. */
bool app_state_lock(uint32_t timeout_ms);
void app_state_unlock(void);

/* Valid only while the lock is held. */
app_state_t *app_state_locked(void);

#ifdef __cplusplus
}
#endif
