#include <stdlib.h>
#include <stdbool.h>
#include "aw9523b.h"
#include "esp_log.h"

static const char *TAG = "aw9523b";

#define AW9523B_I2C_FREQ_HZ  400000
#define AW9523B_I2C_TIMEOUT_MS 100

struct aw9523b_t {
    i2c_master_dev_handle_t dev;
};

esp_err_t aw9523b_create(i2c_master_bus_handle_t bus, aw9523b_handle_t *out)
{
    if (bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    aw9523b_handle_t h = calloc(1, sizeof(struct aw9523b_t));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AW9523B_I2C_ADDR,
        .scl_speed_hz    = AW9523B_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    /* Presence check: the ID register must read 0x23. */
    uint8_t id = 0;
    err = aw9523b_read(h, AW9523B_REG_ID, &id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ID register read failed: %s", esp_err_to_name(err));
        aw9523b_destroy(h);
        return err;
    }
    if (id != AW9523B_ID_VALUE) {
        ESP_LOGE(TAG, "unexpected ID 0x%02x (expected 0x%02x)", id, AW9523B_ID_VALUE);
        aw9523b_destroy(h);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "found, ID=0x%02x", id);

    *out = h;
    return ESP_OK;
}

void aw9523b_destroy(aw9523b_handle_t h)
{
    if (h == NULL) {
        return;
    }
    if (h->dev != NULL) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
}

esp_err_t aw9523b_read(aw9523b_handle_t h, uint8_t reg, uint8_t *val)
{
    if (h == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(h->dev, &reg, 1, val, 1, AW9523B_I2C_TIMEOUT_MS);
}

esp_err_t aw9523b_write(aw9523b_handle_t h, uint8_t reg, uint8_t val)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(h->dev, buf, sizeof(buf), AW9523B_I2C_TIMEOUT_MS);
}

static esp_err_t aw9523b_update_bits(aw9523b_handle_t h, uint8_t reg, uint8_t bits, bool set)
{
    uint8_t cur = 0;
    esp_err_t err = aw9523b_read(h, reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t next = set ? (cur | bits) : (cur & (uint8_t)~bits);
    if (next == cur) {
        return ESP_OK;
    }
    return aw9523b_write(h, reg, next);
}

esp_err_t aw9523b_set_bits(aw9523b_handle_t h, uint8_t reg, uint8_t bits)
{
    return aw9523b_update_bits(h, reg, bits, true);
}

esp_err_t aw9523b_clear_bits(aw9523b_handle_t h, uint8_t reg, uint8_t bits)
{
    return aw9523b_update_bits(h, reg, bits, false);
}

esp_err_t aw9523b_lcd_reset_level(aw9523b_handle_t h, bool level)
{
    return level ? aw9523b_set_bits(h, AW9523B_REG_OUTPUT_P1, AW9523B_P1_LCD_RST)
                 : aw9523b_clear_bits(h, AW9523B_REG_OUTPUT_P1, AW9523B_P1_LCD_RST);
}

esp_err_t aw9523b_touch_reset_level(aw9523b_handle_t h, bool level)
{
    return level ? aw9523b_set_bits(h, AW9523B_REG_OUTPUT_P0, AW9523B_P0_TOUCH_RST)
                 : aw9523b_clear_bits(h, AW9523B_REG_OUTPUT_P0, AW9523B_P0_TOUCH_RST);
}
