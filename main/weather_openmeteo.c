#include "weather_openmeteo.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "openmeteo";

#define HTTP_TIMEOUT_MS 15000
#define MAX_BODY_BYTES  CONFIG_LW_HTTP_MAX_RESPONSE_BYTES

/* Coordinates are stored as microdegrees because Kconfig has no float type;
 * this turns 35653600 into "35.653600". */
static void format_coord(int microdeg, char *buf, size_t len)
{
    const char *sign = (microdeg < 0) ? "-" : "";
    const long abs_v = labs((long)microdeg);
    snprintf(buf, len, "%s%ld.%06ld", sign, abs_v / 1000000, abs_v % 1000000);
}

static void build_url(char *url, size_t len)
{
    char lat[24];
    char lon[24];
    format_coord(CONFIG_LW_LATITUDE_MICRO, lat, sizeof(lat));
    format_coord(CONFIG_LW_LONGITUDE_MICRO, lon, sizeof(lon));

    /* timeformat=unixtime keeps the response small and avoids ISO8601
     * parsing; the values are always UTC regardless of the timezone
     * parameter, which only decides day boundaries (SPEC §2.1). */
    snprintf(url, len,
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s"
             "&hourly=precipitation,precipitation_probability"
             "&timezone=Asia%%2FTokyo"
             "&timeformat=unixtime"
             "&forecast_days=2"
             "&models=%s",
             lat, lon, CONFIG_LW_OPENMETEO_MODEL_NAME);
}

/* Reads the body into a capped buffer. Returns NULL on any failure; the
 * cap exists so a misbehaving endpoint cannot exhaust the heap (SPEC §8.1). */
static char *http_get_body(const char *url, int *out_status)
{
    const esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        /* Certificate verification via the bundled root CAs. Never skipped
         * (SPEC §3.1). */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "client init failed");
        return NULL;
    }

    char *body = NULL;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        goto out;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    *out_status = status;
    ESP_LOGI(TAG, "HTTP %d, content-length %lld", status, (long long)content_length);

    if (status != 200) {
        goto out;
    }
    if (content_length > MAX_BODY_BYTES) {
        ESP_LOGE(TAG, "response too large (%lld > %d)", (long long)content_length, MAX_BODY_BYTES);
        goto out;
    }

    /* Body buffers go to PSRAM: there is 8MB of it and no reason to spend
     * scarce internal RAM on a transient string. */
    body = heap_caps_malloc(MAX_BODY_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (body == NULL) {
        ESP_LOGE(TAG, "out of memory for response body");
        goto out;
    }

    int total = 0;
    while (total < MAX_BODY_BYTES) {
        const int n = esp_http_client_read(client, body + total, MAX_BODY_BYTES - total);
        if (n < 0) {
            ESP_LOGE(TAG, "read error after %d bytes", total);
            free(body);
            body = NULL;
            goto out;
        }
        if (n == 0) {
            break;
        }
        total += n;
    }

    if (total >= MAX_BODY_BYTES) {
        /* Truncated: parsing half a JSON document would fail anyway, and
         * silently using it would be worse. */
        ESP_LOGE(TAG, "response hit the %d byte cap; discarding", MAX_BODY_BYTES);
        free(body);
        body = NULL;
        goto out;
    }

    body[total] = '\0';
    ESP_LOGI(TAG, "read %d bytes", total);

out:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return body;
}

/* Open-Meteo returns null for probabilities it has no value for, and the
 * spec says to treat those as zero rather than dropping the slot. */
static int json_number_or(const cJSON *item, int fallback)
{
    if (item == NULL || cJSON_IsNull(item) || !cJSON_IsNumber(item)) {
        return fallback;
    }
    return (int)item->valuedouble;
}

static float json_float_or(const cJSON *item, float fallback)
{
    if (item == NULL || cJSON_IsNull(item) || !cJSON_IsNumber(item)) {
        return fallback;
    }
    return (float)item->valuedouble;
}

static esp_err_t parse_body(const char *body, openmeteo_result_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t err = ESP_ERR_INVALID_RESPONSE;

    const cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!cJSON_IsObject(hourly)) {
        ESP_LOGE(TAG, "no 'hourly' object");
        goto out;
    }

    const cJSON *times = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    const cJSON *precs = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation");
    const cJSON *pops  = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");

    if (!cJSON_IsArray(times) || !cJSON_IsArray(precs)) {
        ESP_LOGE(TAG, "'time' or 'precipitation' is not an array");
        goto out;
    }

    const int n_times = cJSON_GetArraySize(times);
    const int n_precs = cJSON_GetArraySize(precs);
    const int n_pops  = cJSON_IsArray(pops) ? cJSON_GetArraySize(pops) : 0;

    /* The arrays are supposed to be the same length. Taking the shortest
     * means a mismatched response degrades instead of reading past an end. */
    int n = (n_times < n_precs) ? n_times : n_precs;
    if (n > JUDGE_MAX_SLOTS) {
        n = JUDGE_MAX_SLOTS;
    }
    if (n <= 0) {
        ESP_LOGE(TAG, "empty forecast arrays (time=%d prec=%d)", n_times, n_precs);
        goto out;
    }
    if (n_times != n_precs || (cJSON_IsArray(pops) && n_pops != n_times)) {
        ESP_LOGW(TAG, "array length mismatch (time=%d prec=%d pop=%d); using %d",
                 n_times, n_precs, n_pops, n);
    }

    for (int i = 0; i < n; i++) {
        const cJSON *t_item = cJSON_GetArrayItem(times, i);
        if (!cJSON_IsNumber(t_item)) {
            ESP_LOGE(TAG, "slot %d has a non-numeric time", i);
            goto out;
        }
        out->slots[i].t   = (time_t)t_item->valuedouble;
        out->slots[i].mm  = json_float_or(cJSON_GetArrayItem(precs, i), 0.0f);
        out->slots[i].pop = (i < n_pops) ? json_number_or(cJSON_GetArrayItem(pops, i), 0) : 0;

        if (out->slots[i].mm < 0.0f) {
            out->slots[i].mm = 0.0f;
        }
        if (out->slots[i].pop < 0) {
            out->slots[i].pop = 0;
        } else if (out->slots[i].pop > 100) {
            out->slots[i].pop = 100;
        }
    }
    out->n_slots = n;

    /* Slots are assumed hourly and ascending by the judgement code. Warn
     * loudly rather than silently mis-locating "now" if that ever changes. */
    for (int i = 1; i < n; i++) {
        if (out->slots[i].t - out->slots[i - 1].t != JUDGE_SLOT_SEC) {
            ESP_LOGW(TAG, "slot %d is %lld s after the previous one, expected %d",
                     i, (long long)(out->slots[i].t - out->slots[i - 1].t), JUDGE_SLOT_SEC);
            break;
        }
    }

    err = ESP_OK;

out:
    cJSON_Delete(root);
    return err;
}

esp_err_t openmeteo_fetch(openmeteo_result_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    char url[512];
    build_url(url, sizeof(url));
    /* Open-Meteo needs no key, so the URL is safe to log in full. */
    ESP_LOGI(TAG, "GET %s", url);

    int status = 0;
    char *body = http_get_body(url, &status);
    if (body == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t err = parse_body(body, out);
    free(body);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "parsed %d slots; first: t=%lld mm=%.2f pop=%d",
                 out->n_slots, (long long)out->slots[0].t,
                 (double)out->slots[0].mm, out->slots[0].pop);
    }
    return err;
}
