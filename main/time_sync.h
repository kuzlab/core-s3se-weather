/* System clock: RTC on boot, SNTP once the network is up (SPEC §8.2).
 *
 * The BM8563 keeps time across power cycles so the header shows something
 * plausible before Wi-Fi associates. SNTP then overwrites it, and the
 * corrected time is written back to the RTC.
 */
#pragma once

#include <stdbool.h>
#include "bm8563.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets TZ to JST and seeds the system clock from the RTC. Returns an error
 * if the RTC has no trustworthy time; the clock is then simply unset. */
esp_err_t time_sync_init(bm8563_handle_t rtc);

/* Starts SNTP. Non-blocking; use time_is_valid() to tell when it landed. */
esp_err_t time_sync_start_sntp(void);

/* True once the clock holds a plausible wall time. Judgements must not be
 * made before this: an unset clock puts the current slot in the wrong place
 * (SPEC §8.2). */
bool time_is_valid(void);

/* Writes the current system time back to the RTC. Call after SNTP lands. */
esp_err_t time_sync_store_to_rtc(bm8563_handle_t rtc);

#ifdef __cplusplus
}
#endif
