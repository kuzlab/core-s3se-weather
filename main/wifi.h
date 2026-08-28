/* Wi-Fi with indefinite exponential-backoff reconnect (SPEC §8.1).
 *
 * The device is meant to sit powered on for weeks, so a dropped link must
 * never be "fixed" by rebooting: it retries forever with a widening delay.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_start(void);

bool wifi_is_connected(void);

/* Blocks until the first association succeeds, or the timeout expires.
 * Returns false on timeout; the retry loop keeps running either way. */
bool wifi_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
