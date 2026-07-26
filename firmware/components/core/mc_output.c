#include "mc_output.h"

#include <string.h>

void mc_output_config_default(mc_output_config_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        out->channels[i].function = MC_OUT_FUNC_NONE;
        /* MC_OUT_MODE_ON, not OFF: mode now independently describes a
         * channel's electrical behavior *when* commanded on (plain digital
         * vs PWM-dimmed vs a flasher pattern) rather than being derived
         * from commanded_on the way it once was (mc_output_set()
         * used to force mode to mirror `on` every call — removed, since a
         * flasher-mode channel must keep its mode across on/off toggles).
         * MC_OUT_MODE_ON is the right default for a freshly-assigned
         * channel: ordinary digital on/off until an installer opts a
         * channel into PWM/flasher mode via config. */
        out->channels[i].mode = MC_OUT_MODE_ON;
        out->channels[i].commanded_on = false;
        out->channels[i].name[0] = '\0';
        out->channels[i].pwm_duty_pct = MC_OUTPUT_DEFAULT_PWM_DUTY_PCT;
    }
    out->starter_interlock_input = -1;
    out->brake_switch_input = -1;
    out->turn_auto_cancel_ms = MC_OUTPUT_DEFAULT_TURN_AUTO_CANCEL_MS;
    out->turn_flash_period_ms = MC_OUTPUT_DEFAULT_TURN_FLASH_PERIOD_MS;
    out->brake_flash_pulse_count = MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_COUNT;
    out->brake_flash_pulse_on_ms = MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_ON_MS;
    out->brake_flash_pulse_off_ms = MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_OFF_MS;
}

void mc_output_init(mc_output_engine_t *eng, const mc_output_config_t *config, mc_output_hal_t hal)
{
    memset(eng, 0, sizeof(*eng));
    eng->config = *config;
    eng->hal = hal;
    eng->engine_running = false;
    eng->interlock_engaged = false;
}

/* Whether channel `channel` is driven ON at this exact instant, ignoring
 * lv_cutoff suppression (apply_hal_state()/mc_output_get_actual_state()
 * apply that on top) — i.e. purely "is commanded_on true, and if so, what
 * does this channel's mode say to do with it right now". Shared by both
 * so they can never disagree; mc_diag depends on this staying accurate
 * down to the blink phase, not just commanded_on (see mc_output.h). */
static bool compute_driven_on(const mc_output_engine_t *eng, uint8_t channel, uint32_t now_ms)
{
    const mc_output_channel_config_t *ch = &eng->config.channels[channel];
    if (!ch->commanded_on) {
        return false;
    }
    switch (ch->mode) {
    case MC_OUT_MODE_OFF:
        return false;
    case MC_OUT_MODE_ON:
    case MC_OUT_MODE_PWM:
        return true;
    case MC_OUT_MODE_FLASH_TURN: {
        uint16_t period = eng->config.turn_flash_period_ms;
        if (period == 0) {
            return true; /* malformed config; fail open to solid rather than divide by zero */
        }
        uint16_t half = period / 2;
        if (half == 0) {
            return true;
        }
        return ((now_ms / half) % 2) == 0;
    }
    case MC_OUT_MODE_FLASH_BRAKE: {
        uint8_t count = eng->config.brake_flash_pulse_count;
        uint16_t on_ms = eng->config.brake_flash_pulse_on_ms;
        uint16_t off_ms = eng->config.brake_flash_pulse_off_ms;
        if (count == 0 || (on_ms == 0 && off_ms == 0)) {
            return true; /* no pulses configured: solid immediately */
        }
        uint32_t cycle = (uint32_t)on_ms + off_ms;
        uint32_t total_burst_ms = cycle * count;
        uint32_t elapsed = now_ms - eng->brake_burst_started_ms[channel]; /* wrap-safe */
        if (elapsed >= total_burst_ms) {
            return true; /* burst finished: solid */
        }
        uint32_t phase = (cycle == 0) ? 0 : (elapsed % cycle);
        return phase < on_ms;
    }
    default:
        return true;
    }
}

/* Single place that actually drives the HAL — every caller (restore, a live
 * mc_output_set(), a low-voltage cutoff engaging/recovering, the periodic
 * tick) goes through this so "what's actually energized" has one
 * definition. */
static void apply_hal_state(mc_output_engine_t *eng, uint8_t channel, uint32_t now_ms)
{
    if (eng->hal.set == NULL) {
        return;
    }
    bool driven_on = mc_output_get_actual_state(eng, channel, now_ms);
    const mc_output_channel_config_t *ch = &eng->config.channels[channel];
    if (driven_on && ch->mode == MC_OUT_MODE_PWM && eng->hal.set_duty != NULL) {
        eng->hal.set_duty(channel, ch->pwm_duty_pct, eng->hal.ctx);
        return;
    }
    eng->hal.set(channel, driven_on, eng->hal.ctx);
}

void mc_output_restore_from_config(mc_output_engine_t *eng)
{
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        apply_hal_state(eng, ch, eng->last_tick_ms);
    }
}

static mc_output_result_t validate_starter_command(const mc_output_engine_t *eng, mc_output_source_t source)
{
    if (source == MC_OUT_SRC_REMOTE) {
        return MC_OUT_ERR_STARTER_REMOTE_BLOCKED;
    }
    if (eng->engine_running) {
        return MC_OUT_ERR_STARTER_ENGINE_RUNNING;
    }
    if (eng->config.starter_interlock_input >= 0 && !eng->interlock_engaged) {
        return MC_OUT_ERR_STARTER_INTERLOCK;
    }
    return MC_OUT_OK;
}

/* Sets commanded_on with no turn-signal policy attached (mc_output_set()
 * layers mutual exclusion/auto-cancel on top of this; mc_output_hazard_press()
 * deliberately calls this directly to bypass that policy). Stamps the
 * brake-flasher burst start on a FLASH_BRAKE channel's off->on edge, and
 * drives the HAL immediately. */
static void set_commanded_on_raw(mc_output_engine_t *eng, uint8_t channel, bool on, uint32_t now_ms)
{
    mc_output_channel_config_t *ch = &eng->config.channels[channel];
    bool was_on = ch->commanded_on;
    ch->commanded_on = on;
    if (!was_on && on && ch->mode == MC_OUT_MODE_FLASH_BRAKE) {
        eng->brake_burst_started_ms[channel] = now_ms;
    }
    apply_hal_state(eng, channel, now_ms);
}

mc_output_result_t mc_output_set(mc_output_engine_t *eng, uint8_t channel, bool on, mc_output_source_t source)
{
    if (channel >= MC_OUTPUT_COUNT) {
        return MC_OUT_ERR_BAD_CHANNEL;
    }

    mc_output_channel_config_t *ch_cfg = &eng->config.channels[channel];

    if (on && eng->immobilized &&
        (ch_cfg->function == MC_OUT_FUNC_IGNITION || ch_cfg->function == MC_OUT_FUNC_STARTER)) {
        return MC_OUT_ERR_IMMOBILIZED;
    }

    if (on && ch_cfg->function == MC_OUT_FUNC_STARTER) {
        mc_output_result_t err = validate_starter_command(eng, source);
        if (err != MC_OUT_OK) {
            return err;
        }
    }

    bool is_turn = (ch_cfg->function == MC_OUT_FUNC_TURN_L || ch_cfg->function == MC_OUT_FUNC_TURN_R);

    if (is_turn && on) {
        mc_output_function_t opposite_fn =
            (ch_cfg->function == MC_OUT_FUNC_TURN_L) ? MC_OUT_FUNC_TURN_R : MC_OUT_FUNC_TURN_L;
        int opposite = mc_output_find_channel_by_function(&eng->config, opposite_fn);
        if (opposite >= 0 && opposite != channel && eng->config.channels[opposite].commanded_on) {
            set_commanded_on_raw(eng, (uint8_t)opposite, false, eng->last_tick_ms);
            eng->turn_auto_cancel_deadline_ms[opposite] = 0;
        }
    }

    set_commanded_on_raw(eng, channel, on, eng->last_tick_ms);

    if (is_turn) {
        eng->turn_auto_cancel_deadline_ms[channel] =
            (on && eng->config.turn_auto_cancel_ms > 0) ? eng->last_tick_ms + eng->config.turn_auto_cancel_ms : 0;
    }

    return MC_OUT_OK;
}

bool mc_output_get_state(const mc_output_engine_t *eng, uint8_t channel)
{
    if (channel >= MC_OUTPUT_COUNT) {
        return false;
    }
    return eng->config.channels[channel].commanded_on;
}

bool mc_output_get_actual_state(const mc_output_engine_t *eng, uint8_t channel, uint32_t now_ms)
{
    if (channel >= MC_OUTPUT_COUNT) {
        return false;
    }
    if (eng->lv_cutoff && !mc_output_function_is_essential(eng->config.channels[channel].function)) {
        return false;
    }
    return compute_driven_on(eng, channel, now_ms);
}

void mc_output_hazard_press(mc_output_engine_t *eng, uint32_t now_ms)
{
    int l = mc_output_find_channel_by_function(&eng->config, MC_OUT_FUNC_TURN_L);
    int r = mc_output_find_channel_by_function(&eng->config, MC_OUT_FUNC_TURN_R);
    if (l < 0 && r < 0) {
        return;
    }
    bool both_on = (l < 0 || eng->config.channels[l].commanded_on) &&
                   (r < 0 || eng->config.channels[r].commanded_on);
    bool new_state = !both_on;
    if (l >= 0) {
        set_commanded_on_raw(eng, (uint8_t)l, new_state, now_ms);
        eng->turn_auto_cancel_deadline_ms[l] = 0;
    }
    if (r >= 0) {
        set_commanded_on_raw(eng, (uint8_t)r, new_state, now_ms);
        eng->turn_auto_cancel_deadline_ms[r] = 0;
    }
}

void mc_output_tick(mc_output_engine_t *eng, uint32_t now_ms)
{
    eng->last_tick_ms = now_ms;

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        uint32_t deadline = eng->turn_auto_cancel_deadline_ms[ch];
        if (deadline != 0 && (int32_t)(now_ms - deadline) >= 0) {
            eng->turn_auto_cancel_deadline_ms[ch] = 0;
            set_commanded_on_raw(eng, ch, false, now_ms);
        }
    }

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        apply_hal_state(eng, ch, now_ms);
    }
}

void mc_output_set_engine_running(mc_output_engine_t *eng, bool running)
{
    eng->engine_running = running;
}

void mc_output_set_interlock_engaged(mc_output_engine_t *eng, bool engaged)
{
    eng->interlock_engaged = engaged;
}

void mc_output_set_immobilized(mc_output_engine_t *eng, bool immobilized)
{
    eng->immobilized = immobilized;
}

void mc_output_set_lv_cutoff(mc_output_engine_t *eng, bool cutoff_active)
{
    if (eng->lv_cutoff == cutoff_active) {
        return;
    }
    eng->lv_cutoff = cutoff_active;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        apply_hal_state(eng, ch, eng->last_tick_ms);
    }
}

int mc_output_find_channel_by_function(const mc_output_config_t *config, mc_output_function_t fn)
{
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].function == fn) {
            return i;
        }
    }
    return -1;
}

int mc_output_find_ignition_channel(const mc_output_config_t *config)
{
    return mc_output_find_channel_by_function(config, MC_OUT_FUNC_IGNITION);
}

uint32_t mc_output_config_validate(const mc_output_config_t *config)
{
    uint32_t flags = 0;
    int ignition_count = 0;
    int starter_count = 0;

    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].function == MC_OUT_FUNC_IGNITION) {
            ignition_count++;
        }
        if (config->channels[i].function == MC_OUT_FUNC_STARTER) {
            starter_count++;
        }
    }

    if (ignition_count > 1) {
        flags |= MC_OUT_CFG_MULTIPLE_IGNITION;
    }
    if (starter_count > 1) {
        flags |= MC_OUT_CFG_MULTIPLE_STARTER;
    }

    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].mode == MC_OUT_MODE_PWM &&
            (config->channels[i].pwm_duty_pct < 1 || config->channels[i].pwm_duty_pct > 100)) {
            flags |= MC_OUT_CFG_BAD_PWM_DUTY;
        }
    }
    if (config->turn_flash_period_ms == 0) {
        flags |= MC_OUT_CFG_BAD_TURN_PERIOD;
    }

    return flags;
}
