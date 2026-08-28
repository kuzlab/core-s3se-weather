#include <stdlib.h>
#include "axp2101.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

#define AXP2101_I2C_FREQ_HZ    400000
#define AXP2101_I2C_TIMEOUT_MS 100

/* M5GFX writes 0xBF: ALDO1-4 + BLDO1-2 + DLDO1 on, CPUSLDO off. */
#define AXP2101_LDO_EN0_CORES3SE 0xBF

struct axp2101_t {
    i2c_master_dev_handle_t dev;
};

esp_err_t axp2101_create(i2c_master_bus_handle_t bus, axp2101_handle_t *out)
{
    if (bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    axp2101_handle_t h = calloc(1, sizeof(struct axp2101_t));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = AXP2101_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (err != ESP_OK) {
        free(h);
        return err;
    }

    uint8_t ldo_en = 0;
    err = axp2101_read(h, AXP2101_REG_LDO_EN0, &ldo_en);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe read failed: %s", esp_err_to_name(err));
        axp2101_destroy(h);
        return err;
    }
    ESP_LOGI(TAG, "found, LDO_EN0=0x%02x", ldo_en);

    *out = h;
    return ESP_OK;
}

void axp2101_destroy(axp2101_handle_t h)
{
    if (h == NULL) {
        return;
    }
    if (h->dev != NULL) {
        i2c_master_bus_rm_device(h->dev);
    }
    free(h);
}

esp_err_t axp2101_read(axp2101_handle_t h, uint8_t reg, uint8_t *val)
{
    if (h == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(h->dev, &reg, 1, val, 1, AXP2101_I2C_TIMEOUT_MS);
}

esp_err_t axp2101_write(axp2101_handle_t h, uint8_t reg, uint8_t val)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(h->dev, buf, sizeof(buf), AXP2101_I2C_TIMEOUT_MS);
}

static esp_err_t axp2101_update_bits(axp2101_handle_t h, uint8_t reg, uint8_t bits, bool set)
{
    uint8_t cur = 0;
    esp_err_t err = axp2101_read(h, reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t next = set ? (cur | bits) : (cur & (uint8_t)~bits);
    if (next == cur) {
        return ESP_OK;
    }
    return axp2101_write(h, reg, next);
}

esp_err_t axp2101_set_bits(axp2101_handle_t h, uint8_t reg, uint8_t bits)
{
    return axp2101_update_bits(h, reg, bits, true);
}

esp_err_t axp2101_clear_bits(axp2101_handle_t h, uint8_t reg, uint8_t bits)
{
    return axp2101_update_bits(h, reg, bits, false);
}

esp_err_t axp2101_cores3se_power_on(axp2101_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = axp2101_write(h, AXP2101_REG_LDO_EN0, AXP2101_LDO_EN0_CORES3SE);
    if (err != ESP_OK) {
        return err;
    }
    /* ALDO3 powers the camera on the plain CoreS3. CoreS3 SE has no camera,
     * but M5GFX sets it unconditionally and it is harmless, so keep parity
     * with the known-good sequence rather than inventing a variant. */
    err = axp2101_write(h, AXP2101_REG_ALDO3_VOL, AXP2101_LDO_VOLT_REG(3300));
    if (err != ESP_OK) {
        return err;
    }
    /* ALDO4 powers the TF card slot. */
    return axp2101_write(h, AXP2101_REG_ALDO4_VOL, AXP2101_LDO_VOLT_REG(3300));
}

esp_err_t axp2101_set_backlight(axp2101_handle_t h, uint8_t brightness)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (brightness == 0) {
        return axp2101_clear_bits(h, AXP2101_REG_LDO_EN0, AXP2101_LDO_EN0_DLDO1);
    }

    /* Same mapping M5GFX uses: 1 -> 20 (2.5V), 255 -> 28 (3.3V). */
    const uint8_t volt_reg = (uint8_t)((brightness + 641) >> 5);

    esp_err_t err = axp2101_set_bits(h, AXP2101_REG_LDO_EN0, AXP2101_LDO_EN0_DLDO1);
    if (err != ESP_OK) {
        return err;
    }
    return axp2101_write(h, AXP2101_REG_DLDO1_VOL, volt_reg);
}
