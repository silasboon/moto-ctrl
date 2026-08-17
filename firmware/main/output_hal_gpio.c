#include "output_hal_gpio.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"

static const char *TAG = "output_hal_gpio";
static const gpio_num_t s_out_pins[BOARD_OUTPUT_COUNT] = BOARD_OUTPUT_IN_PINS;

void output_hal_gpio_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BOARD_OUTPUT_COUNT; i++) {
        mask |= (1ULL << s_out_pins[i]);
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

/* --- PWM dimming, lazily attached.
 *
 * PWM dimming is opt-in per channel and off by default (the PWM/flasher
 * rule) — most boards will never call output_hal_gpio_set_duty()
 * at all, so this deliberately does NOT reconfigure every output pin onto
 * LEDC at init time (that would mean every channel, including the common
 * case of a plain digital load, goes through the LEDC peripheral instead of
 * the already-correct, already-tested plain GPIO path — more risk for no
 * benefit). Instead, a channel's pin is claimed onto LEDC only the first
 * time dimming is actually requested for it; a channel that's never used
 * PWM mode always takes the plain gpio_set_level() path below, including
 * every flasher-pattern channel (flashers are always full on/off, never
 * partial duty, so mc_output.c never calls set_duty() for one) — zero LEDC
 * overhead or risk for the common case.
 *
 * Once a channel HAS been attached to LEDC, though, the peripheral owns
 * that pin's signal — a later plain gpio_set_level() call would fight the
 * LEDC hardware rather than reliably override it. So output_hal_gpio_set()
 * below checks the same attachment map: once attached, even an ordinary
 * on/off (e.g. the channel's mode later reverts to plain ON, or it's
 * simply commanded off) is expressed as an LEDC duty of 0 or full-scale,
 * not a raw GPIO write.
 *
 * ESP32-S3's LEDC has 8 channels in LEDC_LOW_SPEED_MODE (its only speed
 * mode — LEDC_HIGH_SPEED_MODE doesn't exist on this target), so at most 8
 * of the 12 output channels can be dimmed concurrently; a 9th concurrent
 * request is refused (logged, falls back to plain digital-on) rather than
 * doing anything unsafe. In practice, dimming more than a handful of
 * channels (headlight, maybe an aux running light) on one bike is an
 * unlikely configuration.
 *
 * ~1kHz / 10-bit resolution: comfortably above any human-visible flicker
 * threshold. Not bench-validated against the BTS7008-2EPA's actual PWM
 * frequency tolerance or for EMI. */
#define OUTPUT_HAL_LEDC_TIMER LEDC_TIMER_0
#define OUTPUT_HAL_LEDC_FREQ_HZ 1000
#define OUTPUT_HAL_LEDC_RES LEDC_TIMER_10_BIT
#define OUTPUT_HAL_LEDC_DUTY_MAX ((1u << 10) - 1u) /* matches OUTPUT_HAL_LEDC_RES */
#define OUTPUT_HAL_LEDC_CHANNEL_COUNT 8

static bool s_ledc_timer_ready = false;
/* channel_slot[i] = the output channel currently occupying LEDC channel i,
 * or -1 if free. */
static int8_t s_ledc_channel_slot[OUTPUT_HAL_LEDC_CHANNEL_COUNT];
/* ledc_channel_for_output[c] = which LEDC channel output channel c is
 * attached to, or -1 if never attached (still plain GPIO). */
static int8_t s_ledc_channel_for_output[BOARD_OUTPUT_COUNT];
static bool s_ledc_maps_initialized = false;

static void ledc_maps_init_once(void)
{
    if (s_ledc_maps_initialized) {
        return;
    }
    for (int i = 0; i < OUTPUT_HAL_LEDC_CHANNEL_COUNT; i++) {
        s_ledc_channel_slot[i] = -1;
    }
    for (int i = 0; i < BOARD_OUTPUT_COUNT; i++) {
        s_ledc_channel_for_output[i] = -1;
    }
    s_ledc_maps_initialized = true;
}

/* Returns the LEDC channel `channel` is (or becomes) attached to, or -1 if
 * none is available (caller falls back to plain digital-on). */
static int attach_channel_to_ledc(uint8_t channel)
{
    ledc_maps_init_once();
    if (s_ledc_channel_for_output[channel] >= 0) {
        return s_ledc_channel_for_output[channel];
    }

    if (!s_ledc_timer_ready) {
        ledc_timer_config_t timer_cfg = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .timer_num = OUTPUT_HAL_LEDC_TIMER,
            .duty_resolution = OUTPUT_HAL_LEDC_RES,
            .freq_hz = OUTPUT_HAL_LEDC_FREQ_HZ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        if (ledc_timer_config(&timer_cfg) != ESP_OK) {
            ESP_LOGE(TAG, "LEDC timer config failed; channel %u stays plain digital", channel);
            return -1;
        }
        s_ledc_timer_ready = true;
    }

    int slot = -1;
    for (int i = 0; i < OUTPUT_HAL_LEDC_CHANNEL_COUNT; i++) {
        if (s_ledc_channel_slot[i] < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        ESP_LOGE(TAG, "no free LEDC channel for output channel %u (max %d concurrent PWM channels); "
                      "falling back to plain digital", channel, OUTPUT_HAL_LEDC_CHANNEL_COUNT);
        return -1;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num = s_out_pins[channel],
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)slot,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = OUTPUT_HAL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    if (ledc_channel_config(&ch_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed for output channel %u; falling back to plain digital", channel);
        return -1;
    }

    s_ledc_channel_slot[slot] = (int8_t)channel;
    s_ledc_channel_for_output[channel] = (int8_t)slot;
    return slot;
}

/* Last state driven per channel, so the log below reports only transitions.
 * Initialised false, matching the real pin state: output_hal_gpio_init()
 * configures every output and the PROFET inputs sit low, so a channel that
 * boots OFF has genuinely not changed and says nothing. */
static bool s_last_driven[BOARD_OUTPUT_COUNT];

static void output_hal_gpio_set(uint8_t channel, bool on, void *ctx)
{
    (void)ctx;
    if (channel >= BOARD_OUTPUT_COUNT) {
        return;
    }

    /* Bench visibility: log which channels the firmware actually drives, with
     * the 1-based OUTn label and GPIO, so "the app says OUT8 but OUT12 lit up"
     * can be settled from the monitor alone.
     *
     * EDGE-TRIGGERED, deliberately. mc_output_tick() re-applies every channel
     * to the HAL every 10ms — that is how blink and flasher phases advance —
     * so logging each write produced ~1200 lines/second of unchanging state
     * and buried everything else. Only transitions are interesting, and at
     * rest this is silent. A blinking indicator still logs twice per blink
     * period, which is the point. */
    if (on != s_last_driven[channel]) {
        ESP_LOGI(TAG, "OUT%u (ch=%u, GPIO%d) -> %s",
                 (unsigned)(channel + 1), channel, (int)s_out_pins[channel],
                 on ? "ON" : "OFF");
        s_last_driven[channel] = on;
    }

    ledc_maps_init_once();
    if (s_ledc_channel_for_output[channel] >= 0) {
        /* This pin has been claimed by LEDC (dimming was used at least once
         * on this channel) — the peripheral owns the signal now, so even a
         * plain on/off has to go through it (full-scale or zero duty)
         * rather than a raw gpio_set_level(), which the LEDC hardware would
         * otherwise fight/override. */
        int ledc_ch = s_ledc_channel_for_output[channel];
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ledc_ch, on ? OUTPUT_HAL_LEDC_DUTY_MAX : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ledc_ch);
        return;
    }
    gpio_set_level(s_out_pins[channel], on ? 1 : 0);
}

static void output_hal_gpio_set_duty(uint8_t channel, uint8_t duty_pct, void *ctx)
{
    (void)ctx;
    if (channel >= BOARD_OUTPUT_COUNT) {
        return;
    }
    if (duty_pct > 100) {
        duty_pct = 100;
    }

    int ledc_ch = attach_channel_to_ledc(channel);
    if (ledc_ch < 0) {
        /* No free LEDC channel — fail safe to plain on rather than silently
         * doing nothing (an installer who opted into dimming still gets a
         * working, just non-dimmed, light rather than a dark one). */
        gpio_set_level(s_out_pins[channel], 1);
        return;
    }

    uint32_t duty = (OUTPUT_HAL_LEDC_DUTY_MAX * duty_pct) / 100u;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ledc_ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)ledc_ch);
}

mc_output_hal_t output_hal_gpio_get(void)
{
    mc_output_hal_t hal = { .set = output_hal_gpio_set, .set_duty = output_hal_gpio_set_duty, .ctx = NULL };
    return hal;
}
