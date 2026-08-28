#include <stdlib.h>
#include <string.h>
#include "bm8563.h"
#include "esp_log.h"

static const char *TAG = "bm8563";

#define BM8563_I2C_FREQ_HZ    400000
#define BM8563_I2C_TIMEOUT_MS 100

#define BM8563_REG_CTRL1   0x00
#define BM8563_REG_CTRL2   0x01
#define BM8563_REG_SECONDS 0x02  /* bit7 = VL (voltage low / time invalid) */

#define BM8563_VL_FLAG     0x80
#define BM8563_CENTURY_BIT 0x80  /* in the month register: set = 19xx */

struct bm8563_t {
    i2c_master_dev_handle_t dev;
};

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0f)); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t bm8563_read_regs(bm8563_handle_t h, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(h->dev, &reg, 1, buf, len, BM8563_I2C_TIMEOUT_MS);
}

static esp_err_t bm8563_write_regs(bm8563_handle_t h, uint8_t reg, const uint8_t *buf, size_t len)
{
    uint8_t tmp[16];
    if (len + 1 > sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    tmp[0] = reg;
    memcpy(&tmp[1], buf, len);
    return i2c_master_transmit(h->dev, tmp, len + 1, BM8563_I2C_TIMEOUT_MS);
}

esp_err_t bm8563_create(i2c_master_bus_handle_t bus, bm8563_handle_t *out)
{
    if (bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bm8563_handle_t h = calloc(1, sizeof(struct bm8563_t));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BM8563_I2C_ADDR,
        .scl_speed_hz    = BM8563_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    /* Normal running mode, no test mode, alarms/timer interrupts off. */
    const uint8_t ctrl[2] = { 0x00, 0x00 };
    err = bm8563_write_regs(h, BM8563_REG_CTRL1, ctrl, sizeof(ctrl));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init write failed: %s", esp_err_to_name(err));
        bm8563_destroy(h);
        return err;
    }
    ESP_LOGI(TAG, "found");

    *out = h;
    return ESP_OK;
}

void bm8563_destroy(bm8563_handle_t h)
{
    if (h == NULL) {
        return;
    }
    if (h->dev != NULL) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
}

esp_err_t bm8563_get_time(bm8563_handle_t h, struct tm *out)
{
    if (h == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[7] = { 0 };
    esp_err_t err = bm8563_read_regs(h, BM8563_REG_SECONDS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd_to_bin(buf[0] & 0x7f);
    out->tm_min  = bcd_to_bin(buf[1] & 0x7f);
    out->tm_hour = bcd_to_bin(buf[2] & 0x3f);
    out->tm_mday = bcd_to_bin(buf[3] & 0x3f);
    out->tm_wday = buf[4] & 0x07;
    out->tm_mon  = bcd_to_bin(buf[5] & 0x1f) - 1;

    const int base_year = (buf[5] & BM8563_CENTURY_BIT) ? 1900 : 2000;
    out->tm_year = base_year + bcd_to_bin(buf[6]) - 1900;
    out->tm_isdst = 0;

    if (buf[0] & BM8563_VL_FLAG) {
        /* The RTC lost power; the values above are meaningless. */
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t bm8563_set_time(bm8563_handle_t h, const struct tm *t)
{
    if (h == NULL || t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int year = t->tm_year + 1900;
    if (year < 1900 || year > 2099) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[7];
    /* Writing seconds with bit7 clear also clears the voltage-low flag. */
    buf[0] = bin_to_bcd((uint8_t)t->tm_sec) & 0x7f;
    buf[1] = bin_to_bcd((uint8_t)t->tm_min);
    buf[2] = bin_to_bcd((uint8_t)t->tm_hour);
    buf[3] = bin_to_bcd((uint8_t)t->tm_mday);
    buf[4] = (uint8_t)(t->tm_wday & 0x07);
    buf[5] = bin_to_bcd((uint8_t)(t->tm_mon + 1));
    if (year < 2000) {
        buf[5] |= BM8563_CENTURY_BIT;
        buf[6] = bin_to_bcd((uint8_t)(year - 1900));
    } else {
        buf[6] = bin_to_bcd((uint8_t)(year - 2000));
    }

    return bm8563_write_regs(h, BM8563_REG_SECONDS, buf, sizeof(buf));
}
