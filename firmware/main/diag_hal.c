#include "diag_hal.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "mc_diag_hal";

static const gpio_num_t s_den_pins[BOARD_OUTPUT_COUNT] = BOARD_OUTPUT_DEN_PINS;

static adc_oneshot_unit_handle_t s_adc1;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

/* IS line RC filter is 2.2kOhm/1nF (~2.2us time constant); this leaves a
 * generous margin for the DSEL mux + ADC sample-and-hold to settle after
 * switching DEN. Not bench-verified — see diag_hal.h. */
#define DEN_SETTLE_US 50

void diag_hal_init(void)
{
    uint64_t mask = 1ULL << BOARD_DSEL_PIN;
    for (int i = 0; i < BOARD_OUTPUT_COUNT; i++) {
        mask |= (1ULL << s_den_pins[i]);
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        /* GPIO46 (U4's DEN, shared by OUT5/OUT6) is a strapping pin — never
         * enable an internal pullup on it, per PINOUT.md / board_config's
         * warning. Every DEN/DSEL line is push-pull output only. */
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BOARD_DSEL_PIN, 0);
    for (int i = 0; i < BOARD_OUTPUT_COUNT; i++) {
        gpio_set_level(s_den_pins[i], 0); /* PINOUT.md: only one DEN may be high at a time */
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc1) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed; current/battery readings will be 0");
        return;
    }

    /* PINOUT.md: 12dB attenuation for both PROFET_IS (ADC1_CH8/GPIO9) and
     * VSENSE_BAT (ADC1_CH0/GPIO1). */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc1, ADC_CHANNEL_8, &chan_cfg);
    adc_oneshot_config_channel(s_adc1, ADC_CHANNEL_0, &chan_cfg);

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK);
    if (!s_cali_ok) {
        ESP_LOGW(TAG, "ADC calibration scheme unavailable on this chip/eFuse; falling back to uncalibrated raw counts");
    }
}

static uint16_t read_mv(adc_channel_t chan)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc1, chan, &raw) != ESP_OK) {
        return 0;
    }
    if (s_cali_ok) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            return (uint16_t)(mv < 0 ? 0 : mv);
        }
    }
    /* Last-resort fallback if the calibration scheme is unavailable: a
     * coarse linear estimate over 12-bit full scale at 12dB attenuation
     * (~3100mV nominal). mc_diag_calib_t's gain/offset (bench calibration,
     * MC_OP_DIAG_SET_CALIB) is the real correction path — this only avoids
     * fabricating a number when neither is available. */
    return (uint16_t)((raw * 3100) / 4095);
}

static uint16_t diag_hal_read_channel_mv(uint8_t channel, void *ctx)
{
    (void)ctx;
    if (channel >= BOARD_OUTPUT_COUNT) {
        return 0;
    }

    /* PINOUT.md "Diagnostics readout procedure": 0 = OUT-odd/IN0 channel,
     * 1 = OUT-even/IN1 channel. 0-indexed channel N is OUT(N+1), so an even
     * N is an odd-numbered OUT (the device's first-listed IN pin). */
    uint8_t dsel = (uint8_t)(channel % 2);
    gpio_set_level(BOARD_DSEL_PIN, dsel);
    gpio_set_level(s_den_pins[channel], 1);
    esp_rom_delay_us(DEN_SETTLE_US);

    uint16_t mv = read_mv(ADC_CHANNEL_8);

    gpio_set_level(s_den_pins[channel], 0);
    return mv;
}

static uint16_t diag_hal_read_vbat_mv(void *ctx)
{
    (void)ctx;
    return read_mv(ADC_CHANNEL_0);
}

mc_diag_hal_t diag_hal_get(void)
{
    mc_diag_hal_t hal = {
        .read_channel_mv = diag_hal_read_channel_mv,
        .read_vbat_mv = diag_hal_read_vbat_mv,
        .ctx = NULL,
    };
    return hal;
}
