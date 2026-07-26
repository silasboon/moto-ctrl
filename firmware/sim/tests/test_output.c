#include "mc_output.h"

#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t channel;
    bool on;
    bool is_duty;     /* true if this call was hal.set_duty(), not hal.set() */
    uint8_t duty_pct; /* valid when is_duty */
} recorded_call_t;

typedef struct {
    recorded_call_t calls[32];
    int count;
} recorder_t;

static void hal_set(uint8_t channel, bool on, void *ctx)
{
    recorder_t *r = (recorder_t *)ctx;
    if (r->count < 32) {
        r->calls[r->count].channel = channel;
        r->calls[r->count].on = on;
        r->calls[r->count].is_duty = false;
        r->calls[r->count].duty_pct = 0;
        r->count++;
    }
}

static void hal_set_duty(uint8_t channel, uint8_t duty_pct, void *ctx)
{
    recorder_t *r = (recorder_t *)ctx;
    if (r->count < 32) {
        r->calls[r->count].channel = channel;
        r->calls[r->count].is_duty = true;
        r->calls[r->count].duty_pct = duty_pct;
        r->count++;
    }
}

static void test_default_config_is_all_off(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        assert(mc_output_get_state(&eng, ch) == false);
    }
}

static void test_set_plain_channel(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_HEADLIGHT_LO;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_result_t res = mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL);
    assert(res == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 0) == true);
    assert(rec.count == 1);
    assert(rec.calls[0].channel == 0);
    assert(rec.calls[0].on == true);
}

static void test_starter_blocked_from_remote(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[5].function = MC_OUT_FUNC_STARTER;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_result_t res = mc_output_set(&eng, 5, true, MC_OUT_SRC_REMOTE);
    assert(res == MC_OUT_ERR_STARTER_REMOTE_BLOCKED);
    assert(mc_output_get_state(&eng, 5) == false);
    assert(rec.count == 0);
}

static void test_starter_blocked_while_engine_running(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[5].function = MC_OUT_FUNC_STARTER;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);
    mc_output_set_engine_running(&eng, true);

    mc_output_result_t res = mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL);
    assert(res == MC_OUT_ERR_STARTER_ENGINE_RUNNING);
    assert(rec.count == 0);
}

static void test_starter_requires_interlock_when_configured(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[5].function = MC_OUT_FUNC_STARTER;
    cfg.starter_interlock_input = 0;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_result_t res = mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL);
    assert(res == MC_OUT_ERR_STARTER_INTERLOCK);
    assert(rec.count == 0);

    mc_output_set_interlock_engaged(&eng, true);
    res = mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL);
    assert(res == MC_OUT_OK);
    assert(rec.count == 1);
}

static void test_starter_off_never_blocked(void)
{
    /* Turning the starter OFF must never be blocked by these guards —
     * only commanding it ON is starter-protected. */
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[5].function = MC_OUT_FUNC_STARTER;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);
    mc_output_set_engine_running(&eng, true);

    mc_output_result_t res = mc_output_set(&eng, 5, false, MC_OUT_SRC_REMOTE);
    assert(res == MC_OUT_OK);
    assert(rec.count == 1);
    assert(rec.calls[0].on == false);
}

static void test_restore_from_config_applies_all_channels(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[1].commanded_on = true;
    cfg.channels[7].commanded_on = true;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_restore_from_config(&eng);

    assert(rec.count == MC_OUTPUT_COUNT);
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        bool expected = (ch == 1 || ch == 7);
        assert(rec.calls[ch].channel == ch);
        assert(rec.calls[ch].on == expected);
    }
}

static void test_config_validate_flags_duplicates(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    assert(mc_output_config_validate(&cfg) == MC_OUT_CFG_OK);

    cfg.channels[0].function = MC_OUT_FUNC_IGNITION;
    cfg.channels[1].function = MC_OUT_FUNC_IGNITION;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_MULTIPLE_IGNITION);

    mc_output_config_default(&cfg);
    cfg.channels[2].function = MC_OUT_FUNC_STARTER;
    cfg.channels[3].function = MC_OUT_FUNC_STARTER;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_MULTIPLE_STARTER);
}

static void test_bad_channel_rejected(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, MC_OUTPUT_COUNT, true, MC_OUT_SRC_LOCAL) == MC_OUT_ERR_BAD_CHANNEL);
    assert(mc_output_get_state(&eng, MC_OUTPUT_COUNT) == false);
}

/* --- Low-voltage cutoff (AGENTS.md #7) --- */

static void test_lv_cutoff_suppresses_nonessential_preserves_commanded_on(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L; /* non-essential */
    cfg.channels[1].function = MC_OUT_FUNC_HORN;   /* non-essential */

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 1, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(rec.count == 2 && rec.calls[0].on == true && rec.calls[1].on == true);

    rec.count = 0;
    mc_output_set_lv_cutoff(&eng, true);
    /* Every channel re-applied on the transition; the two ON ones drive OFF. */
    assert(rec.count == MC_OUTPUT_COUNT);
    for (int i = 0; i < rec.count; i++) {
        if (rec.calls[i].channel == 0 || rec.calls[i].channel == 1) {
            assert(rec.calls[i].on == false);
        } else {
            assert(rec.calls[i].on == false); /* everything else was already off */
        }
    }

    /* commanded_on (intent) survives the cutoff untouched. */
    assert(mc_output_get_state(&eng, 0) == true);
    assert(mc_output_get_state(&eng, 1) == true);
    assert(mc_output_get_actual_state(&eng, 0, 0) == false);
    assert(mc_output_get_actual_state(&eng, 1, 0) == false);

    rec.count = 0;
    mc_output_set_lv_cutoff(&eng, false);
    assert(rec.count == MC_OUTPUT_COUNT);
    assert(mc_output_get_actual_state(&eng, 0, 0) == true);
    assert(mc_output_get_actual_state(&eng, 1, 0) == true);
}

static void test_lv_cutoff_never_suppresses_essential(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_IGNITION;
    cfg.channels[1].function = MC_OUT_FUNC_BRAKE;
    cfg.channels[2].function = MC_OUT_FUNC_HEADLIGHT_HI;
    cfg.channels[3].function = MC_OUT_FUNC_HEADLIGHT_LO;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);
    for (uint8_t ch = 0; ch < 4; ch++) {
        assert(mc_output_set(&eng, ch, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    }

    mc_output_set_lv_cutoff(&eng, true);
    for (uint8_t ch = 0; ch < 4; ch++) {
        assert(mc_output_get_actual_state(&eng, ch, 0) == true);
    }
}

static void test_lv_cutoff_no_op_when_already_set(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_set_lv_cutoff(&eng, true);
    rec.count = 0;
    mc_output_set_lv_cutoff(&eng, true); /* already true: no re-apply */
    assert(rec.count == 0);
}

static void test_function_is_essential(void)
{
    assert(mc_output_function_is_essential(MC_OUT_FUNC_IGNITION));
    assert(mc_output_function_is_essential(MC_OUT_FUNC_BRAKE));
    assert(mc_output_function_is_essential(MC_OUT_FUNC_HEADLIGHT_HI));
    assert(mc_output_function_is_essential(MC_OUT_FUNC_HEADLIGHT_LO));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_TURN_L));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_TURN_R));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_HORN));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_STARTER));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_AUX));
    assert(!mc_output_function_is_essential(MC_OUT_FUNC_NONE));
}

/* --- Turn-signal mutual exclusion + auto-cancel --- */

static void test_default_config_mode_is_on(void)
{
    /* Mode independently describes electrical behavior (plain digital
     * vs PWM vs a flasher pattern) rather than being derived from
     * commanded_on — see mc_output_config_default()'s
     * comment. MC_OUT_MODE_ON must be the default so a freshly-assigned
     * channel behaves as ordinary digital on/off until explicitly opted
     * into something else. */
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        assert(cfg.channels[i].mode == MC_OUT_MODE_ON);
    }
}

static void test_find_channel_by_function(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    assert(mc_output_find_channel_by_function(&cfg, MC_OUT_FUNC_BRAKE) == -1);
    cfg.channels[4].function = MC_OUT_FUNC_BRAKE;
    assert(mc_output_find_channel_by_function(&cfg, MC_OUT_FUNC_BRAKE) == 4);
}

static void test_turn_mutual_exclusion(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.channels[1].function = MC_OUT_FUNC_TURN_R;
    cfg.turn_auto_cancel_ms = 0; /* isolate mutual exclusion from auto-cancel */

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 0) == true);

    rec.count = 0;
    assert(mc_output_set(&eng, 1, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 1) == true);
    assert(mc_output_get_state(&eng, 0) == false); /* opposite side cancelled */

    bool saw_ch0_off = false, saw_ch1_on = false;
    for (int i = 0; i < rec.count; i++) {
        if (rec.calls[i].channel == 0 && rec.calls[i].on == false) {
            saw_ch0_off = true;
        }
        if (rec.calls[i].channel == 1 && rec.calls[i].on == true) {
            saw_ch1_on = true;
        }
    }
    assert(saw_ch0_off && saw_ch1_on);
}

static void test_turn_auto_cancel_expiry(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.turn_auto_cancel_ms = 1000;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_tick(&eng, 0);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 0) == true);

    mc_output_tick(&eng, 500);
    assert(mc_output_get_state(&eng, 0) == true); /* not yet */

    mc_output_tick(&eng, 1000);
    assert(mc_output_get_state(&eng, 0) == false); /* auto-cancelled */
}

static void test_turn_auto_cancel_disabled_when_zero(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.turn_auto_cancel_ms = 0;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_tick(&eng, 0);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    mc_output_tick(&eng, 1000000); /* huge elapsed time, still never cancels */
    assert(mc_output_get_state(&eng, 0) == true);
}

static void test_hazard_toggle_bypasses_mutual_exclusion(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.channels[1].function = MC_OUT_FUNC_TURN_R;
    cfg.turn_auto_cancel_ms = 5000;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_tick(&eng, 0);

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_get_state(&eng, 0) == true);
    assert(mc_output_get_state(&eng, 1) == true); /* both on -- no mutual-exclusion cancellation */

    /* No auto-cancel timer armed by hazard: still on well past turn_auto_cancel_ms. */
    mc_output_tick(&eng, 60000);
    assert(mc_output_get_state(&eng, 0) == true);
    assert(mc_output_get_state(&eng, 1) == true);

    mc_output_hazard_press(&eng, 60000); /* both on -> toggles both off */
    assert(mc_output_get_state(&eng, 0) == false);
    assert(mc_output_get_state(&eng, 1) == false);
}

static void test_hazard_noop_without_turn_channels(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_hazard_press(&eng, 0);
    assert(rec.count == 0);
}

/* --- MC_OUT_MODE_FLASH_TURN blink phase --- */

static void test_flash_turn_blink_phase(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_TURN;
    cfg.turn_flash_period_ms = 200; /* half-period = 100ms */
    cfg.turn_auto_cancel_ms = 0;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    assert(mc_output_get_actual_state(&eng, 0, 0) == true);
    assert(mc_output_get_actual_state(&eng, 0, 50) == true);
    assert(mc_output_get_actual_state(&eng, 0, 100) == false);
    assert(mc_output_get_actual_state(&eng, 0, 199) == false);
    assert(mc_output_get_actual_state(&eng, 0, 200) == true);
    assert(mc_output_get_actual_state(&eng, 0, 1000000000u) == true); /* stateless clock: still in sync far later */

    /* commanded_on false overrides any blink phase. */
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_actual_state(&eng, 0, 50) == false);
}

static void test_flash_turn_channels_stay_in_sync(void)
{
    /* Both sides use the same stateless global blink clock -- hazards need
     * TURN_L and TURN_R to blink together. */
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_TURN;
    cfg.channels[1].function = MC_OUT_FUNC_TURN_R;
    cfg.channels[1].mode = MC_OUT_MODE_FLASH_TURN;
    cfg.turn_flash_period_ms = 200;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_hazard_press(&eng, 0);

    for (uint32_t t = 0; t < 800; t += 37) {
        assert(mc_output_get_actual_state(&eng, 0, t) == mc_output_get_actual_state(&eng, 1, t));
    }
}

/* --- MC_OUT_MODE_FLASH_BRAKE attention burst --- */

static void test_flash_brake_burst_then_solid(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_BRAKE;
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_BRAKE;
    cfg.brake_flash_pulse_count = 2;
    cfg.brake_flash_pulse_on_ms = 100;
    cfg.brake_flash_pulse_off_ms = 50;
    /* total burst = 2 * (100+50) = 300ms */

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_tick(&eng, 1000);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK); /* burst starts at t=1000 */

    assert(mc_output_get_actual_state(&eng, 0, 1000) == true);  /* pulse 1 on: [1000,1100) */
    assert(mc_output_get_actual_state(&eng, 0, 1099) == true);
    assert(mc_output_get_actual_state(&eng, 0, 1100) == false); /* pulse 1 off: [1100,1150) */
    assert(mc_output_get_actual_state(&eng, 0, 1149) == false);
    assert(mc_output_get_actual_state(&eng, 0, 1150) == true);  /* pulse 2 on: [1150,1250) */
    assert(mc_output_get_actual_state(&eng, 0, 1249) == true);
    assert(mc_output_get_actual_state(&eng, 0, 1250) == false); /* pulse 2 off: [1250,1300) */
    assert(mc_output_get_actual_state(&eng, 0, 1299) == false);
    assert(mc_output_get_actual_state(&eng, 0, 1300) == true);  /* burst finished: solid */
    assert(mc_output_get_actual_state(&eng, 0, 5000) == true);  /* still solid, arbitrarily later */
}

static void test_flash_brake_off_is_immediate_and_restarts_burst(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_BRAKE;
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_BRAKE;
    cfg.brake_flash_pulse_count = 3;
    cfg.brake_flash_pulse_on_ms = 100;
    cfg.brake_flash_pulse_off_ms = 50;
    /* pulse cycle = 150ms, total burst = 450ms */

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    mc_output_tick(&eng, 1000);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK); /* burst starts at 1000 */
    assert(mc_output_get_actual_state(&eng, 0, 1350) == true); /* well past the burst: solid */

    mc_output_tick(&eng, 2000);
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK); /* off: always immediate, no pattern */
    assert(mc_output_get_actual_state(&eng, 0, 2000) == false);

    mc_output_tick(&eng, 5000);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK); /* a NEW burst starts at 5000 */
    /* If the burst had NOT restarted (still measured from t=1000), 4100ms
     * would have elapsed by t=5100 -- long past the 450ms burst, so it'd
     * read solid (true). Reading false here proves the burst timer really
     * did reset to this new on-edge. */
    assert(mc_output_get_actual_state(&eng, 0, 5010) == true);  /* first pulse, on-phase */
    assert(mc_output_get_actual_state(&eng, 0, 5100) == false); /* first pulse, off-phase */
}

/* --- PWM dimming --- */

static void test_pwm_duty_uses_hal_set_duty(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_AUX;
    cfg.channels[0].mode = MC_OUT_MODE_PWM;
    cfg.channels[0].pwm_duty_pct = 40;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .set_duty = hal_set_duty, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(rec.count == 1);
    assert(rec.calls[0].is_duty == true);
    assert(rec.calls[0].duty_pct == 40);

    rec.count = 0;
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(rec.count == 1);
    assert(rec.calls[0].is_duty == false); /* off always goes through plain set(), never set_duty */
    assert(rec.calls[0].on == false);
}

static void test_pwm_falls_back_to_plain_set_without_hal_duty(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_AUX;
    cfg.channels[0].mode = MC_OUT_MODE_PWM;
    cfg.channels[0].pwm_duty_pct = 40;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec }; /* no set_duty */
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(rec.count == 1);
    assert(rec.calls[0].is_duty == false);
    assert(rec.calls[0].on == true);
}

/* --- lv_cutoff x flasher-mode interactions --- */

static void test_lv_cutoff_suppresses_flash_turn_regardless_of_phase(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_TURN_L; /* non-essential */
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_TURN;
    cfg.turn_flash_period_ms = 200;
    cfg.turn_auto_cancel_ms = 0;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_actual_state(&eng, 0, 0) == true); /* on-phase, no cutoff yet */

    mc_output_set_lv_cutoff(&eng, true);
    assert(mc_output_get_actual_state(&eng, 0, 0) == false);   /* would be on-phase, but suppressed */
    assert(mc_output_get_actual_state(&eng, 0, 100) == false); /* would be off-phase anyway */
}

static void test_lv_cutoff_never_suppresses_brake_mid_burst(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].function = MC_OUT_FUNC_BRAKE; /* essential */
    cfg.channels[0].mode = MC_OUT_MODE_FLASH_BRAKE;
    cfg.brake_flash_pulse_count = 3;
    cfg.brake_flash_pulse_on_ms = 100;
    cfg.brake_flash_pulse_off_ms = 50;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_tick(&eng, 0);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    mc_output_set_lv_cutoff(&eng, true);
    assert(mc_output_get_actual_state(&eng, 0, 10) == true);   /* pulse 1 on-phase: essential, never suppressed */
    assert(mc_output_get_actual_state(&eng, 0, 110) == false); /* pulse 1 off-phase: that's the pattern, not cutoff */
}

/* --- Config validation --- */

static void test_config_validate_flags_bad_pwm_duty(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].mode = MC_OUT_MODE_PWM;
    cfg.channels[0].pwm_duty_pct = 0;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_PWM_DUTY);

    cfg.channels[0].pwm_duty_pct = 101;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_PWM_DUTY);

    cfg.channels[0].pwm_duty_pct = 50;
    assert(!(mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_PWM_DUTY));
}

static void test_config_validate_flags_zero_turn_period(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    assert(!(mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_TURN_PERIOD));
    cfg.turn_flash_period_ms = 0;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_TURN_PERIOD);
}

int main(void)
{
    test_default_config_is_all_off();
    test_set_plain_channel();
    test_starter_blocked_from_remote();
    test_starter_blocked_while_engine_running();
    test_starter_requires_interlock_when_configured();
    test_starter_off_never_blocked();
    test_restore_from_config_applies_all_channels();
    test_config_validate_flags_duplicates();
    test_bad_channel_rejected();
    test_lv_cutoff_suppresses_nonessential_preserves_commanded_on();
    test_lv_cutoff_never_suppresses_essential();
    test_lv_cutoff_no_op_when_already_set();
    test_function_is_essential();

    test_default_config_mode_is_on();
    test_find_channel_by_function();
    test_turn_mutual_exclusion();
    test_turn_auto_cancel_expiry();
    test_turn_auto_cancel_disabled_when_zero();
    test_hazard_toggle_bypasses_mutual_exclusion();
    test_hazard_noop_without_turn_channels();
    test_flash_turn_blink_phase();
    test_flash_turn_channels_stay_in_sync();
    test_flash_brake_burst_then_solid();
    test_flash_brake_off_is_immediate_and_restarts_burst();
    test_pwm_duty_uses_hal_set_duty();
    test_pwm_falls_back_to_plain_set_without_hal_duty();
    test_lv_cutoff_suppresses_flash_turn_regardless_of_phase();
    test_lv_cutoff_never_suppresses_brake_mid_burst();
    test_config_validate_flags_bad_pwm_duty();
    test_config_validate_flags_zero_turn_period();
    return 0;
}
