/* Beep output through the AW88298 amplifier (SPEC §1.4).
 *
 * Register values come from m5stack/M5Unified's CoreS3 speaker callback; see
 * docs/HARDWARE.md §4. The AW88298 uses 16-bit registers, unlike every other
 * chip on this bus.
 *
 * Audio is a secondary feature: every failure here is logged and swallowed
 * so the display keeps working (SPEC §4.2).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cores3se_bsp.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp.audio";

#define AW88298_I2C_ADDR   0x36
#define AW88298_I2C_FREQ   400000
#define AW88298_TIMEOUT_MS 100

/* I2S pins for CoreS3 SE. Note DOUT is GPIO13: SPEC §4.2's table has DOUT
 * and DIN swapped relative to M5Unified, and the spec itself notes the
 * documentation disagrees with itself here (docs/HARDWARE.md §4). */
#define AUDIO_I2S_PORT  I2S_NUM_1
#define AUDIO_MCLK_GPIO 0
#define AUDIO_BCLK_GPIO 34
#define AUDIO_WS_GPIO   33
#define AUDIO_DOUT_GPIO 13

#define AUDIO_SAMPLE_RATE 16000

#ifdef CONFIG_LW_BEEP_VOLUME
#define AUDIO_VOLUME_STEPS CONFIG_LW_BEEP_VOLUME
#else
#define AUDIO_VOLUME_STEPS 3
#endif

/* Amplitude out of a 32767 full scale, one entry per volume step.
 *
 * The steps are about 4dB apart rather than evenly spaced, because loudness
 * is perceived logarithmically: a linear scale crams every usable quiet
 * setting into its bottom one or two steps and leaves nothing to choose
 * between. Step 10 is as hard as this amplifier is driven here; step 1 is
 * roughly a whisper across a room. */
static const int AUDIO_VOLUME_TABLE[10] = {
    142, 226, 358, 568, 900, 1427, 2262, 3586, 5684, 9000
};

#define AUDIO_AMPLITUDE (AUDIO_VOLUME_TABLE[AUDIO_VOLUME_STEPS - 1])

static i2c_master_dev_handle_t s_aw88298;
static i2s_chan_handle_t       s_tx;
static bool                    s_ready;

static esp_err_t aw88298_write(uint8_t reg, uint16_t value)
{
    /* Big-endian 16-bit registers. */
    const uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(s_aw88298, buf, sizeof(buf), AW88298_TIMEOUT_MS);
}

/* M5Unified picks register 0x06 from a table indexed by sample rate. */
static uint16_t aw88298_rate_reg(int sample_rate)
{
    static const uint8_t rate_tbl[] = { 4, 5, 6, 8, 10, 11, 15, 20, 22, 44 };
    const size_t rate = (size_t)((sample_rate + 1102) / 2205);

    size_t idx = 0;
    while (idx < sizeof(rate_tbl) && rate > rate_tbl[idx]) {
        idx++;
    }
    if (idx >= sizeof(rate_tbl)) {
        idx = sizeof(rate_tbl) - 1;
    }
    return (uint16_t)(idx | 0x14C0);  /* I2SBCK=0, 16 bits x 2 channels */
}

esp_err_t bsp_audio_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_bus();
    aw9523b_handle_t expander = bsp_aw9523b();
    if (bus == NULL || expander == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* AW9523B P0_2 gates the amplifier's supply. */
    esp_err_t err = aw9523b_set_bits(expander, AW9523B_REG_OUTPUT_P0, AW9523B_P0_BUS_OUT_EN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "amp power enable failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AW88298_I2C_ADDR,
        .scl_speed_hz    = AW88298_I2C_FREQ,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_aw88298);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AW88298 not addressable: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = aw88298_write(0x61, 0x0673)) != ESP_OK ||   /* boost disabled */
        (err = aw88298_write(0x04, 0x4040)) != ESP_OK ||   /* I2SEN=1 PWDN=0 */
        (err = aw88298_write(0x05, 0x0008)) != ESP_OK ||
        (err = aw88298_write(0x06, aw88298_rate_reg(AUDIO_SAMPLE_RATE))) != ESP_OK ||
        (err = aw88298_write(0x0C, 0x0064)) != ESP_OK) {   /* volume */
        ESP_LOGW(TAG, "AW88298 setup failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_GPIO,
            .bclk = AUDIO_BCLK_GPIO,
            .ws   = AUDIO_WS_GPIO,
            .dout = AUDIO_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s std init failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = NULL;
        return err;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "AW88298 up (%d Hz, volume %d/10, amplitude %d)",
             AUDIO_SAMPLE_RATE, AUDIO_VOLUME_STEPS, AUDIO_AMPLITUDE);
    return ESP_OK;
}

static esp_err_t play_tone(int freq_hz, int duration_ms)
{
    const size_t n_frames = (size_t)AUDIO_SAMPLE_RATE * duration_ms / 1000;
    int16_t *buf = malloc(n_frames * 2 * sizeof(int16_t));
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const float step = 2.0f * (float)M_PI * (float)freq_hz / (float)AUDIO_SAMPLE_RATE;
    for (size_t i = 0; i < n_frames; i++) {
        /* Ramp the first and last 2ms so the tone does not start or stop on
         * a step, which the speaker renders as an audible click. */
        const size_t ramp = AUDIO_SAMPLE_RATE * 2 / 1000;
        float gain = 1.0f;
        if (i < ramp) {
            gain = (float)i / (float)ramp;
        } else if (i + ramp > n_frames) {
            gain = (float)(n_frames - i) / (float)ramp;
        }

        const int16_t s = (int16_t)(sinf(step * (float)i) * AUDIO_AMPLITUDE * gain);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }

    size_t written = 0;
    const esp_err_t err = i2s_channel_write(s_tx, buf, n_frames * 2 * sizeof(int16_t),
                                            &written, pdMS_TO_TICKS(duration_ms + 500));
    free(buf);
    return err;
}

esp_err_t bsp_beep_alert(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Two short notes, rising. Deliberately brief: this fires on a change of
     * verdict, not continuously (SPEC §1.4). */
    esp_err_t err = play_tone(1760, 90);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(60));
        err = play_tone(2349, 110);
    }
    return err;
}

bool bsp_audio_available(void)
{
    return s_ready;
}
