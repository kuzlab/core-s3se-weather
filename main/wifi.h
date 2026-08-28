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

/* Coarse states the UI can turn into a sentence. "not connected" on its own
 * is useless to someone standing in front of the device: they need to know
 * whether to fix the SSID, the password, or where the box is sitting. */
typedef enum {
    WIFI_UI_DISABLED = 0,   /* no SSID configured */
    WIFI_UI_CONNECTING,
    WIFI_UI_CONNECTED,
    WIFI_UI_NO_AP,          /* SSID not visible: wrong name, 5GHz, or range */
    WIFI_UI_BAD_AUTH,       /* password rejected */
    WIFI_UI_OTHER_FAILURE,
} wifi_ui_state_t;

esp_err_t wifi_start(void);

bool wifi_is_connected(void);

wifi_ui_state_t wifi_ui_state(void);

/* Raw esp_wifi disconnect reason, for the log. */
int wifi_last_reason(void);

/* Blocks until the first association succeeds, or the timeout expires.
 * Returns false on timeout; the retry loop keeps running either way. */
bool wifi_wait_connected(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
