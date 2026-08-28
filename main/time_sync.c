#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "time";

/* Anything before this is either an unset clock (1970) or a corrupt RTC
 * read. Chosen well after the code was written and well before any plausible
 * deployment date. */
#define TIME_PLAUSIBLE_FROM 1735689600  /* 2025-01-01T00:00:00Z */

/* struct tm (UTC) -> unixtime. mktime() would apply the JST offset, and
 * timegm() is not dependably declared by the toolchain's newlib, so the
 * conversion is done here. Days-from-civil per Howard Hinnant's algorithm,
 * valid for any date this device will ever see. */
static time_t utc_tm_to_time(const struct tm *tm)
{
    int y = tm->tm_year + 1900;
    const unsigned m = (unsigned)tm->tm_mon + 1;
    const unsigned d = (unsigned)tm->tm_mday;

    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;

    return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

esp_err_t time_sync_init(bm8563_handle_t rtc)
{
    setenv("TZ", "JST-9", 1);
    tzset();

    if (rtc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm rtc_tm;
    const esp_err_t err = bm8563_get_time(rtc, &rtc_tm);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC time not usable (%s); waiting for SNTP",
                 esp_err_to_name(err));
        return err;
    }

    /* The RTC holds UTC. */
    const time_t t = utc_tm_to_time(&rtc_tm);
    if (t < TIME_PLAUSIBLE_FROM) {
        ESP_LOGW(TAG, "RTC time implausible (%lld); waiting for SNTP", (long long)t);
        return ESP_ERR_INVALID_STATE;
    }

    const struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    char buf[32];
    struct tm local;
    localtime_r(&t, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "clock seeded from RTC: %s JST", buf);
    return ESP_OK;
}

esp_err_t time_sync_start_sntp(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        2, ESP_SNTP_SERVER_LIST("ntp.nict.jp", "pool.ntp.org"));
    cfg.start = true;
    cfg.server_from_dhcp = false;
    /* Re-sync periodically so a weeks-long uptime does not drift. */
    cfg.sync_cb = NULL;

    const esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SNTP started (ntp.nict.jp, pool.ntp.org)");
    return ESP_OK;
}

bool time_is_valid(void)
{
    return time(NULL) >= TIME_PLAUSIBLE_FROM;
}

esp_err_t time_sync_store_to_rtc(bm8563_handle_t rtc)
{
    if (rtc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!time_is_valid()) {
        return ESP_ERR_INVALID_STATE;
    }

    const time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);

    const esp_err_t err = bm8563_set_time(rtc, &utc);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "RTC updated from system clock");
    } else {
        ESP_LOGW(TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    return err;
}
