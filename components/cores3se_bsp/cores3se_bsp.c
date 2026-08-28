#include <stdbool.h>
#include "cores3se_bsp.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

/* Touch controller version registers (docs/HARDWARE.md §2-1) */
#define FT5X06_REG_DEVICE_MODE 0x00
#define FT5X06_REG_CIPHER      0xA3
#define FT5X06_REG_FIRMID      0xA6
#define FT5X06_REG_VENDID      0xA8
#define FT5X06_VENDID_M5STACK  0x11
#define FT5X06_FIRMID_ILI9342C 0x10
#define FT5X06_FIRMID_ILI9342E 0x12
/* M5GFX reads these registers at 100kHz, not the bus's usual 400kHz. */
#define FT5X06_VERSION_I2C_FREQ_HZ 100000

static i2c_master_bus_handle_t s_bus;
static aw9523b_handle_t        s_aw9523b;
static axp2101_handle_t        s_axp2101;
static bm8563_handle_t         s_bm8563;

static esp_err_t bsp_i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                     = BSP_I2C_PORT,
        .sda_io_num                   = BSP_I2C_SDA_GPIO,
        .scl_io_num                   = BSP_I2C_SCL_GPIO,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "internal I2C up (SDA=%d SCL=%d)", BSP_I2C_SDA_GPIO, BSP_I2C_SCL_GPIO);
    return ESP_OK;
}

/* M5GFX enables the VBUS 5V output only when the SPI bus pull-ups read low,
 * which indicates nothing is holding those lines up. Reproduce that check so
 * the board behaves the same way in both wiring situations. */
static bool bsp_spi_pullups_absent(void)
{
    const gpio_num_t pins[] = { BSP_LCD_DC_GPIO, BSP_LCD_SCLK_GPIO, BSP_LCD_MOSI_GPIO };
    const size_t n = sizeof(pins) / sizeof(pins[0]);

    uint64_t mask = 0;
    for (size_t i = 0; i < n; i++) {
        mask |= 1ULL << pins[i];
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        return false;
    }
    /* Give the weak internal pull-ups time to settle before sampling. */
    vTaskDelay(pdMS_TO_TICKS(1));

    int level_sum = 0;
    for (size_t i = 0; i < n; i++) {
        level_sum += gpio_get_level(pins[i]);
    }
    for (size_t i = 0; i < n; i++) {
        gpio_reset_pin(pins[i]);
    }

    ESP_LOGI(TAG, "SPI pin pull-up probe: level_sum=%d", level_sum);
    return level_sum == 0;
}

static esp_err_t bsp_aw9523b_power_setup(void)
{
    const bool need_vbus_out = bsp_spi_pullups_absent();

    const uint8_t out_p0 = need_vbus_out ? 0b00000111 : 0b00000101;
    const uint8_t out_p1 = need_vbus_out ? 0b10000011 : 0b00000011;

    esp_err_t err = aw9523b_set_bits(s_aw9523b, AW9523B_REG_OUTPUT_P0, out_p0);
    if (err != ESP_OK) {
        return err;
    }
    err = aw9523b_set_bits(s_aw9523b, AW9523B_REG_OUTPUT_P1, out_p1);
    if (err != ESP_OK) {
        return err;
    }
    /* P0_3/P0_4 and P1_2(TOUCH_INT)/P1_3 are inputs; everything else outputs. */
    err = aw9523b_write(s_aw9523b, AW9523B_REG_CONFIG_P0, 0b00011000);
    if (err != ESP_OK) {
        return err;
    }
    err = aw9523b_write(s_aw9523b, AW9523B_REG_CONFIG_P1, 0b00001100);
    if (err != ESP_OK) {
        return err;
    }
    /* Port 0 push-pull. */
    err = aw9523b_write(s_aw9523b, AW9523B_REG_GCR, 0b00010000);
    if (err != ESP_OK) {
        return err;
    }
    /* Both ports in GPIO mode rather than LED constant-current mode. */
    err = aw9523b_write(s_aw9523b, AW9523B_REG_LEDMODE_P0, 0xFF);
    if (err != ESP_OK) {
        return err;
    }
    return aw9523b_write(s_aw9523b, AW9523B_REG_LEDMODE_P1, 0xFF);
}

esp_err_t bsp_init(void)
{
    esp_err_t err = bsp_i2c_init();
    if (err != ESP_OK) {
        return err;
    }

    /* Scan before powering rails so the log shows which devices are alive on
     * cold boot versus which only appear once the PMIC is configured. */
    ESP_LOGI(TAG, "--- I2C scan (before power-on sequence) ---");
    bsp_i2c_scan_log();

    err = aw9523b_create(s_bus, &s_aw9523b);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW9523B init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = bsp_aw9523b_power_setup();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW9523B port setup failed: %s", esp_err_to_name(err));
        return err;
    }

    err = axp2101_create(s_bus, &s_axp2101);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = axp2101_cores3se_power_on(s_axp2101);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 power-on failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Rails need a moment before the downstream devices answer on I2C. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Release both resets; the LCD and touch panels stay held otherwise. */
    err = aw9523b_touch_reset_level(s_aw9523b, false);
    if (err == ESP_OK) {
        err = aw9523b_lcd_reset_level(s_aw9523b, false);
    }
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    err = aw9523b_touch_reset_level(s_aw9523b, true);
    if (err == ESP_OK) {
        err = aw9523b_lcd_reset_level(s_aw9523b, true);
    }
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    /* The RTC is optional for bring-up: a failure here must not stop the app,
     * because SNTP can still supply the time once Wi-Fi is up (SPEC §8.2). */
    if (bm8563_create(s_bus, &s_bm8563) != ESP_OK) {
        ESP_LOGW(TAG, "BM8563 not available; running without RTC backup");
        s_bm8563 = NULL;
    }

    ESP_LOGI(TAG, "--- I2C scan (after power-on sequence) ---");
    bsp_i2c_scan_log();

    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_bus(void)  { return s_bus; }
aw9523b_handle_t        bsp_aw9523b(void)  { return s_aw9523b; }
axp2101_handle_t        bsp_axp2101(void)  { return s_axp2101; }
bm8563_handle_t         bsp_bm8563(void)   { return s_bm8563; }

esp_err_t bsp_backlight_set(uint8_t brightness)
{
    if (s_axp2101 == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return axp2101_set_backlight(s_axp2101, brightness);
}

void bsp_i2c_scan_log(void)
{
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus not initialised");
        return;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            const char *name = "?";
            switch (addr) {
                case 0x34: name = "AXP2101 (PMIC)";        break;
                case 0x36: name = "AW88298 (spk amp)";     break;
                case 0x38: name = "FT6336U (touch)";       break;
                case 0x40: name = "ES7210 (mic codec)";    break;
                case 0x51: name = "BM8563 (RTC)";          break;
                case 0x58: name = "AW9523B (IO expander)"; break;
                default:                                   break;
            }
            ESP_LOGI(TAG, "  0x%02x  %s", addr, name);
            found++;
        }
    }
    ESP_LOGI(TAG, "  %d device(s) responded", found);
}

bsp_panel_variant_t bsp_probe_panel_variant(void)
{
    if (s_bus == NULL) {
        return BSP_PANEL_UNKNOWN;
    }

    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_TOUCH_I2C_ADDR,
        .scl_speed_hz    = FT5X06_VERSION_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &dev) != ESP_OK) {
        return BSP_PANEL_UNKNOWN;
    }

    bsp_panel_variant_t variant = BSP_PANEL_UNKNOWN;
    uint8_t cipher = 0, firmid = 0, vendid = 0;

    /* M5GFX retries because the touch IC needs time after reset release. */
    for (int retry = 0; retry < 5 && variant == BSP_PANEL_UNKNOWN; retry++) {
        const uint8_t work_mode[2] = { FT5X06_REG_DEVICE_MODE, 0x00 };
        if (i2c_master_transmit(dev, work_mode, sizeof(work_mode), 100) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        const uint8_t regs[3] = { FT5X06_REG_CIPHER, FT5X06_REG_FIRMID, FT5X06_REG_VENDID };
        uint8_t vals[3] = { 0 };
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            if (i2c_master_transmit_receive(dev, &regs[i], 1, &vals[i], 1, 100) != ESP_OK) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        cipher = vals[0];
        firmid = vals[1];
        vendid = vals[2];

        if (vendid == FT5X06_VENDID_M5STACK) {
            if (firmid == FT5X06_FIRMID_ILI9342C) {
                variant = BSP_PANEL_ILI9342C;
            } else if (firmid == FT5X06_FIRMID_ILI9342E) {
                variant = BSP_PANEL_ILI9342E;
            }
        }
        if (variant == BSP_PANEL_UNKNOWN) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    i2c_master_bus_rm_device(dev);

    const char *name = (variant == BSP_PANEL_ILI9342C) ? "ILI9342C"
                     : (variant == BSP_PANEL_ILI9342E) ? "ILI9342E"
                     : "unknown";
    if (variant == BSP_PANEL_UNKNOWN) {
        ESP_LOGW(TAG, "panel variant undetermined (CIPHER=0x%02x FIRMID=0x%02x VENDID=0x%02x)"
                      " -- will assume ILI9342C", cipher, firmid, vendid);
    } else {
        ESP_LOGI(TAG, "panel=%s (CIPHER=0x%02x FIRMID=0x%02x VENDID=0x%02x)",
                 name, cipher, firmid, vendid);
    }
    return variant;
}
