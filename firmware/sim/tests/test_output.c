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
    cfg.channels[0].essential = true;

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
    cfg.channels[5].is_starter = true;

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
    cfg.channels[5].is_starter = true;

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
    cfg.channels[5].is_starter = true;
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
    cfg.channels[5].is_starter = true;

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

    cfg.channels[0].is_ignition = true; cfg.channels[0].essential = true;
    cfg.channels[1].is_ignition = true; cfg.channels[1].essential = true;
    assert(mc_output_config_validate(&cfg) & MC_OUT_CFG_MULTIPLE_IGNITION);

    mc_output_config_default(&cfg);
    cfg.channels[2].is_starter = true;
    cfg.channels[3].is_starter = true;
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

/* --- Low-voltage cutoff --- */

static void test_lv_cutoff_suppresses_nonessential_preserves_commanded_on(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true; /* non-essential */
    cfg.channels[1].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;   /* non-essential */

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
    cfg.channels[0].is_ignition = true; cfg.channels[0].essential = true;
    cfg.channels[1].is_brake = true; cfg.channels[1].essential = true;
    cfg.channels[2].essential = true;
    cfg.channels[3].essential = true;

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

/* The essential set is now explicit per channel rather than derived from a
 * function tag — the change that keeps ride-safe failure enforceable once the
 * headlight tags went away. */
/* mc_power's hold-awake gate. The blink case is the one that matters: a
 * BLINK channel is dark for half its cycle, and reporting "nothing active"
 * during the off phase would let the loop idle down and stall the tick that
 * turns it back on. */
static void test_any_active_tracks_commanded_and_driven(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_tick(&eng, 0);
    assert(!mc_output_any_active(&eng, 0));

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_any_active(&eng, 0));

    /* Half a blink period in: actually dark, still active. */
    uint32_t half = cfg.turn_flash_period_ms / 2u;
    mc_output_tick(&eng, half);
    assert(!mc_output_get_actual_state(&eng, 0, half));
    assert(mc_output_any_active(&eng, half));

    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    mc_output_tick(&eng, half + 10u);
    assert(!mc_output_any_active(&eng, half + 10u));
}

static void test_channel_is_essential(void)
{
    mc_output_channel_config_t ch;

    memset(&ch, 0, sizeof(ch));
    ch.essential = true;
    assert(mc_output_channel_is_essential(&ch));

    /* Ignition and brake are essential even if the flag was never ticked:
     * ride-safe failure does not leave those two to a config mistake. */
    memset(&ch, 0, sizeof(ch));
    ch.is_ignition = true;
    assert(mc_output_channel_is_essential(&ch));
    memset(&ch, 0, sizeof(ch));
    ch.is_brake = true;
    assert(mc_output_channel_is_essential(&ch));

    /* Indicators, hazard members and the starter are all sheddable. */
    memset(&ch, 0, sizeof(ch));
    ch.indicator = MC_INDICATOR_LEFT;
    assert(!mc_output_channel_is_essential(&ch));
    memset(&ch, 0, sizeof(ch));
    ch.hazard_member = true;
    assert(!mc_output_channel_is_essential(&ch));
    memset(&ch, 0, sizeof(ch));
    ch.is_starter = true;
    assert(!mc_output_channel_is_essential(&ch));

    memset(&ch, 0, sizeof(ch));
    assert(!mc_output_channel_is_essential(&ch));
}

/* --- Turn-signal mutual exclusion + auto-cancel --- */

static void test_default_config_mode_is_on(void)
{
    /* Behaviour independently describes how a channel acts when triggered
     * (latching, momentary, blink, flasher) rather than being derived from
     * commanded_on — see mc_output_config_default()'s comment.
     * MC_OUT_BEHAVIOUR_TOGGLE must be the default so a freshly-assigned
     * channel behaves as ordinary on/off until explicitly opted into
     * something else. */
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        assert(cfg.channels[i].behaviour == MC_OUT_BEHAVIOUR_TOGGLE);
    }
}

static void test_find_channel_by_role(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    assert(mc_output_find_brake_channel(&cfg) == -1);
    assert(mc_output_find_ignition_channel(&cfg) == -1);
    assert(mc_output_find_indicator_channel(&cfg, MC_INDICATOR_LEFT) == -1);

    cfg.channels[4].is_brake = true;
    cfg.channels[2].is_ignition = true;
    cfg.channels[6].indicator = MC_INDICATOR_LEFT;
    cfg.channels[7].indicator = MC_INDICATOR_RIGHT;

    assert(mc_output_find_brake_channel(&cfg) == 4);
    assert(mc_output_find_ignition_channel(&cfg) == 2);
    assert(mc_output_find_indicator_channel(&cfg, MC_INDICATOR_LEFT) == 6);
    assert(mc_output_find_indicator_channel(&cfg, MC_INDICATOR_RIGHT) == 7);
    /* NONE never resolves to a channel, even though most channels have it. */
    assert(mc_output_find_indicator_channel(&cfg, MC_INDICATOR_NONE) == -1);
}

static void test_turn_mutual_exclusion(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT; cfg.channels[1].hazard_member = true;
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
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
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
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
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
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT; cfg.channels[1].hazard_member = true;
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

/* --- MC_OUT_BEHAVIOUR_BLINK blink phase --- */

static void test_flash_turn_blink_phase(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
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
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT; cfg.channels[1].hazard_member = true;
    cfg.channels[1].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.turn_flash_period_ms = 200;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_hazard_press(&eng, 0);

    for (uint32_t t = 0; t < 800; t += 37) {
        assert(mc_output_get_actual_state(&eng, 0, t) == mc_output_get_actual_state(&eng, 1, t));
    }
}

/* --- MC_OUT_BEHAVIOUR_FLASHER attention burst --- */

static void test_flash_brake_burst_then_solid(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].is_brake = true; cfg.channels[0].essential = true;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_FLASHER;
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
    cfg.channels[0].is_brake = true; cfg.channels[0].essential = true;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_FLASHER;
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
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
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
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
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
    cfg.channels[0].indicator = MC_INDICATOR_LEFT; cfg.channels[0].hazard_member = true; /* non-essential */
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
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
    cfg.channels[0].is_brake = true; cfg.channels[0].essential = true; /* essential */
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_FLASHER;
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
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
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

/* starter protection: the starter must be inhibited while the engine is running.
 * The command-time guard only covers "don't start cranking now"; the case
 * that wrecks a starter motor is being already engaged when the engine
 * catches, with the rider still holding the button. */
static void test_starter_is_cut_when_engine_starts(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[4].is_starter = true;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    /* Cranking: local (button) source, engine not yet running, no interlock. */
    assert(mc_output_set(&eng, 4, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 4) == true);

    /* Engine catches. */
    mc_output_set_engine_running(&eng, true);
    assert(mc_output_get_state(&eng, 4) == false);

    /* And the HAL was actually driven low, not just the flag cleared. */
    bool saw_off = false;
    for (int i = 0; i < rec.count; i++) {
        if (rec.calls[i].channel == 4 && !rec.calls[i].is_duty && !rec.calls[i].on) {
            saw_off = true;
        }
    }
    assert(saw_off);

    /* It also must not be re-commandable while still running. */
    assert(mc_output_set(&eng, 4, true, MC_OUT_SRC_LOCAL) == MC_OUT_ERR_STARTER_ENGINE_RUNNING);
    assert(mc_output_get_state(&eng, 4) == false);
}

/* Only the starter is cut — an engine starting must not disturb anything
 * else, least of all ignition or lights (ride-safe failure). */
static void test_engine_start_does_not_disturb_other_channels(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[0].is_ignition = true; cfg.channels[0].essential = true;
    cfg.channels[1].essential = true;
    cfg.channels[4].is_starter = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 1, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 4, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    mc_output_set_engine_running(&eng, true);

    assert(mc_output_get_state(&eng, 0) == true);  /* ignition stays live */
    assert(mc_output_get_state(&eng, 1) == true);  /* headlight stays live */
    assert(mc_output_get_state(&eng, 4) == false); /* starter dropped */
}

/* The cut is edge-triggered, so a channel commanded on later while the engine
 * is still running is refused by the command guard rather than silently
 * cut — and repeated set_engine_running(true) calls must not thrash. */
static void test_engine_running_cut_is_edge_triggered(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[4].is_starter = true;
    cfg.channels[7].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;

    recorder_t rec = {0};
    mc_output_hal_t hal = { .set = hal_set, .ctx = &rec };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);

    mc_output_set_engine_running(&eng, true);
    assert(mc_output_set(&eng, 7, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    rec.count = 0;
    mc_output_set_engine_running(&eng, true); /* already running: no-op */
    for (int i = 0; i < rec.count; i++) {
        assert(!(rec.calls[i].channel == 7 && !rec.calls[i].on));
    }
    assert(mc_output_get_state(&eng, 7) == true);
}

/* `momentary` is config the platform tick loop acts on, not something this
 * engine interprets — but it must survive init and not change any of the
 * engine's own behaviour. */
static void test_momentary_flag_is_inert_inside_the_engine(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[3].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[3].behaviour = MC_OUT_BEHAVIOUR_MOMENTARY;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    assert((eng.config.channels[3].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY));

    /* Still an ordinary commanded_on transition either way. */
    assert(mc_output_set(&eng, 3, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == true);
    assert(mc_output_set(&eng, 3, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == false);

    /* And it defaults off, so existing configs keep latching behaviour. */
    mc_output_config_default(&cfg);
    assert((cfg.channels[3].behaviour != MC_OUT_BEHAVIOUR_MOMENTARY));
}

/* The reported bug: a DRL joined to the hazard group stayed solid on while
 * the indicators flashed beside it, because blinking came from the channel's
 * own behaviour and a DRL is a plain toggle. Hazard mode now overrides a
 * member's behaviour for as long as the hazards are running. */
static void test_hazard_blinks_every_member_whatever_its_behaviour(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700; /* 350ms on, 350ms off */
    cfg.channels[0].indicator = MC_INDICATOR_LEFT;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[0].hazard_member = true;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT;
    cfg.channels[1].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[1].hazard_member = true;
    /* The DRL: a plain solid light that merely joins the group. */
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_get_state(&eng, 0) == true);
    assert(mc_output_get_state(&eng, 5) == true);

    /* All three share one blink phase: on together, off together. */
    assert(mc_output_get_actual_state(&eng, 0, 0) == true);
    assert(mc_output_get_actual_state(&eng, 1, 0) == true);
    assert(mc_output_get_actual_state(&eng, 5, 0) == true);

    assert(mc_output_get_actual_state(&eng, 0, 350) == false);
    assert(mc_output_get_actual_state(&eng, 1, 350) == false);
    assert(mc_output_get_actual_state(&eng, 5, 350) == false); /* was the bug */

    assert(mc_output_get_actual_state(&eng, 5, 700) == true);
}

/* ...but only while hazards are running. Switched on by itself, the same DRL
 * is a steady light — which is why forcing the rider to set behaviour=blink
 * would have been the wrong fix. */
static void test_hazard_member_is_solid_when_used_normally(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_actual_state(&eng, 5, 0) == true);
    assert(mc_output_get_actual_state(&eng, 5, 350) == true);   /* solid */
    assert(mc_output_get_actual_state(&eng, 5, 700) == true);
}

/* Turning the hazards off must leave hazard mode, so a later unrelated
 * switch-on of a member is steady again rather than mysteriously blinking. */
static void test_hazard_mode_clears_on_second_press(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_get_actual_state(&eng, 5, 350) == false); /* blinking */

    mc_output_hazard_press(&eng, 0); /* hazards off */
    assert(mc_output_get_state(&eng, 5) == false);

    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_actual_state(&eng, 5, 350) == true);  /* solid again */
}

/* Signalling a turn while hazards run ends hazard mode — any deliberate
 * command to a member does — and every other member goes back to where it was
 * before the hazards, not to off and not left latched on. */
static void test_explicit_command_to_a_member_ends_hazard_mode(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[0].indicator = MC_INDICATOR_LEFT;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[0].hazard_member = true;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT;
    cfg.channels[1].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[1].hazard_member = true;
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_get_actual_state(&eng, 5, 350) == false); /* blinking */

    /* Rider signals left out of the hazard stop. */
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_hazard_active(&eng) == false);
    /* The DRL was off before the hazards, so off is where it belongs. */
    assert(mc_output_get_state(&eng, 5) == false);
    /* And the far indicator releases, rather than staying latched on and
     * blinking — which looked exactly like the hazards had never stopped. */
    assert(mc_output_get_state(&eng, 1) == false);
    /* The signalled one keeps blinking: its own behaviour, not hazards. */
    assert(mc_output_get_state(&eng, 0) == true);
    assert(mc_output_get_actual_state(&eng, 0, 350) == false);
}

/* The reported bug: a DRL that was already lit went dark when the hazards
 * were switched off, because ending them forced every member off rather than
 * back to where it started. */
static void test_hazards_restore_a_member_that_was_already_on(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[0].indicator = MC_INDICATOR_LEFT;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[0].hazard_member = true;
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true; /* the DRL */

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    /* DRL on, riding normally: solid, because hazards aren't running. */
    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_actual_state(&eng, 5, 350) == true);

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == true);
    assert(mc_output_get_actual_state(&eng, 5, 350) == false); /* now blinking */

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == false);
    /* Back to lit and solid — the rider asked for the hazards to stop, not
     * for their running light to go out. */
    assert(mc_output_get_state(&eng, 5) == true);
    assert(mc_output_get_actual_state(&eng, 5, 350) == true);
    /* The indicator, off beforehand, stays off. */
    assert(mc_output_get_state(&eng, 0) == false);
}

/* With a member left on, "is the group lit" is no longer the same question as
 * "are the hazards running" — so a press must key on hazard mode itself or it
 * inverts, and the next press would stop hazards that never started. */
static void test_hazards_start_even_when_a_member_is_already_lit(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[5].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.channels[5].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    /* A single-member group, already lit: "all members on" was true here, so
     * the old all_on test would have read this press as "stop". */
    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == true);
    assert(mc_output_get_actual_state(&eng, 5, 350) == false); /* blinking */

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == false);
    assert(mc_output_get_state(&eng, 5) == true);
}

/* An indicator interrupted mid-signal by a hazard stop comes back with a
 * fresh auto-cancel window, not latched on for the rest of the ride. */
static void test_restored_indicator_rearms_auto_cancel(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.turn_auto_cancel_ms = 5000;
    cfg.channels[0].indicator = MC_INDICATOR_LEFT;
    cfg.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.channels[0].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_tick(&eng, 0);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    mc_output_hazard_press(&eng, 1000);
    mc_output_hazard_press(&eng, 2000); /* restored to on, re-armed from 2000 */
    assert(mc_output_get_state(&eng, 0) == true);

    mc_output_tick(&eng, 6000);
    assert(mc_output_get_state(&eng, 0) == true); /* 4s in, still signalling */
    mc_output_tick(&eng, 7500);
    assert(mc_output_get_state(&eng, 0) == false); /* expired */
}

/* --- schema_version 7: ignition companions (a key turned to "on") --- */

/* Sets up ch0 = ignition, ch5 + ch6 = companions, ch7 = uninvolved. */
static void companion_config(mc_output_config_t *cfg)
{
    mc_output_config_default(cfg);
    cfg->channels[0].is_ignition = true;
    cfg->channels[5].on_with_ignition = true;
    cfg->channels[6].on_with_ignition = true;
}

static void test_ignition_on_lights_its_companions(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_get_state(&eng, 5) == false);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);

    assert(mc_output_get_state(&eng, 5) == true);
    assert(mc_output_get_state(&eng, 6) == true);
    assert(mc_output_get_state(&eng, 7) == false); /* not flagged, untouched */
}

static void test_ignition_off_drops_its_companions(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);

    assert(mc_output_get_state(&eng, 5) == false);
    assert(mc_output_get_state(&eng, 6) == false);
}

/* The whole reason this is edge-triggered rather than held: a rider must be
 * able to switch a DRL off mid-ride without killing the ignition to do it. */
static void test_companion_switched_off_by_hand_stays_off(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_set(&eng, 5, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 5) == false);

    /* Ticking, and re-commanding an already-on ignition, must not undo it. */
    mc_output_tick(&eng, 1000);
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 5) == false);
    assert(mc_output_get_state(&eng, 6) == true); /* the other one is unaffected */
}

/* The reported bug: hi/lo beam stayed lit after the ignition was switched
 * off, because the off-sweep only visited channels flagged on_with_ignition.
 * The flag says what comes ON with the key; everything goes off with it. */
static void test_ignition_off_drops_every_channel_not_just_companions(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);
    /* An alternating headlight pair, deliberately NOT flagged — a rider
     * switches these by hand. */
    cfg.channels[3].alternate_channel = 4;
    cfg.channels[4].alternate_channel = 3;
    cfg.channels[7].essential = true; /* essential is a cutoff rule, not a key rule */

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 7, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == true);
    assert(mc_output_get_state(&eng, 7) == true);

    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        assert(mc_output_get_state(&eng, ch) == false);
    }
}

/* Killing the ignition while the hazards run must not leave the flag set:
 * the status wire would report running hazards with every member dark, and
 * the next press would read as "stop". */
static void test_ignition_off_ends_hazard_mode(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[0].is_ignition = true;
    cfg.channels[1].indicator = MC_INDICATOR_LEFT;
    cfg.channels[1].hazard_member = true;
    cfg.channels[2].indicator = MC_INDICATOR_RIGHT;
    cfg.channels[2].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == true);

    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_hazard_active(&eng) == false);
    assert(mc_output_get_state(&eng, 1) == false);
    assert(mc_output_get_state(&eng, 2) == false);

    /* ...and hazards can still be run with the key off, which is the whole
     * point of a hazard switch on a parked bike. */
    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == true);
    assert(mc_output_get_state(&eng, 1) == true);
}

/* Only a real off-edge sweeps. Re-commanding an already-off ignition must not
 * keep stamping outputs off underneath a rider who just switched one on. */
static void test_ignition_off_sweep_is_edge_triggered(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);

    /* Key off, rider switches an aux circuit on deliberately. */
    assert(mc_output_set(&eng, 7, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 0, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 7) == true);

    mc_output_tick(&eng, 5000);
    assert(mc_output_get_state(&eng, 7) == true);
}

/* ride-safe failure: restore replays exactly what was last commanded. Synthesising
 * an ignition edge at boot would light companions the rider had switched off
 * before parking. */
static void test_restore_does_not_fire_the_ignition_edge(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);
    cfg.channels[0].commanded_on = true;  /* ignition was live when parked */
    cfg.channels[5].commanded_on = false; /* ...but this was switched off */

    recorder_t rec = {0};
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){ .set = hal_set, .ctx = &rec });
    mc_output_restore_from_config(&eng);

    assert(mc_output_get_state(&eng, 5) == false);
    assert(mc_output_get_state(&eng, 0) == true);
}

/* A locked bike refuses the ignition, so nothing downstream of it may light
 * either — the companions must not become a way around the immobilizer. */
static void test_immobilized_ignition_lights_no_companions(void)
{
    mc_output_config_t cfg;
    companion_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    mc_output_set_immobilized(&eng, true);

    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_ERR_IMMOBILIZED);
    assert(mc_output_get_state(&eng, 5) == false);
    assert(mc_output_get_state(&eng, 6) == false);
}

/* --- schema_version 7: alternating pairs (hi/lo beam, DRL colours) --- */

static void pair_config(mc_output_config_t *cfg)
{
    mc_output_config_default(cfg);
    cfg->channels[3].alternate_channel = 4;
    cfg->channels[4].alternate_channel = 3;
}

static void test_lighting_one_of_a_pair_puts_the_other_out(void)
{
    mc_output_config_t cfg;
    pair_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_set(&eng, 3, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == true);
    assert(mc_output_get_state(&eng, 4) == false);

    /* Exclusion is symmetric, and applies to an app command too. */
    assert(mc_output_set(&eng, 4, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == false);
    assert(mc_output_get_state(&eng, 4) == true);
}

static void test_alternate_press_steps_between_members(void)
{
    mc_output_config_t cfg;
    pair_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    /* From cold, the pressed channel lights. */
    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == true);
    assert(mc_output_get_state(&eng, 4) == false);

    /* Then it steps back and forth, never landing on both-off. */
    for (int i = 0; i < 4; i++) {
        assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
        bool three = mc_output_get_state(&eng, 3);
        bool four = mc_output_get_state(&eng, 4);
        assert(three != four); /* exactly one lit, every single press */
    }
}

/* "Never off" is a property of the press action, not of the pair: the app
 * (or a binding aimed at the channel) can still black the pair out. */
static void test_direct_command_can_still_turn_a_pair_off(void)
{
    mc_output_config_t cfg;
    pair_config(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&eng, 3, false, MC_OUT_SRC_REMOTE) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == false);
    assert(mc_output_get_state(&eng, 4) == false);

    /* And a press from that state lights the pressed one again. */
    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_get_state(&eng, 3) == true);
}

static void test_alternate_press_without_a_partner_is_rejected(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_LOCAL) == MC_OUT_ERR_BAD_CHANNEL);
    assert(mc_output_get_state(&eng, 3) == false);
}

/* The starter guards sit in mc_output_set(), which the press helper goes
 * through — so a pair can't be used to sneak the starter on from the app. */
static void test_alternate_press_still_obeys_the_starter_guards(void)
{
    mc_output_config_t cfg;
    pair_config(&cfg);
    cfg.channels[3].is_starter = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});

    assert(mc_output_alternate_press(&eng, 3, MC_OUT_SRC_REMOTE) ==
           MC_OUT_ERR_STARTER_REMOTE_BLOCKED);
    assert(mc_output_get_state(&eng, 3) == false);
}

/* A one-way link would leave both beams lit: A-on cancels B, but B-on
 * wouldn't cancel A. Refuse the config rather than half-apply it. */
static void test_config_validate_flags_asymmetric_pairs(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[3].alternate_channel = 4; /* ...and ch4 names nobody */
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_ALTERNATE) != 0);

    cfg.channels[4].alternate_channel = 3;
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_ALTERNATE) == 0);

    cfg.channels[3].alternate_channel = 3; /* itself */
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_ALTERNATE) != 0);

    cfg.channels[3].alternate_channel = MC_OUTPUT_COUNT; /* out of range */
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_BAD_ALTERNATE) != 0);
}

static void test_config_validate_flags_pair_fighting_over_ignition(void)
{
    mc_output_config_t cfg;
    pair_config(&cfg);
    cfg.channels[3].on_with_ignition = true;
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_ALTERNATE_BOTH_IGNITION) == 0);

    cfg.channels[4].on_with_ignition = true;
    assert((mc_output_config_validate(&cfg) & MC_OUT_CFG_ALTERNATE_BOTH_IGNITION) != 0);
}

/* --- schema_version 7: hazard state is reportable --- */

static void test_hazard_active_tracks_the_group(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.turn_flash_period_ms = 700;
    cfg.channels[0].indicator = MC_INDICATOR_LEFT;
    cfg.channels[0].hazard_member = true;
    cfg.channels[1].indicator = MC_INDICATOR_RIGHT;
    cfg.channels[1].hazard_member = true;

    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, (mc_output_hal_t){0});
    assert(mc_output_hazard_active(&eng) == false);

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == true);
    /* The point of the flag: the driven state alternates while this holds
     * steady, so a client can't infer it from the output mask. */
    assert(mc_output_get_actual_state(&eng, 0, 0) == true);
    assert(mc_output_get_actual_state(&eng, 0, 350) == false);
    assert(mc_output_hazard_active(&eng) == true);

    mc_output_hazard_press(&eng, 0);
    assert(mc_output_hazard_active(&eng) == false);
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
    test_any_active_tracks_commanded_and_driven();
    test_channel_is_essential();
    test_hazard_blinks_every_member_whatever_its_behaviour();
    test_hazard_member_is_solid_when_used_normally();
    test_hazard_mode_clears_on_second_press();
    test_explicit_command_to_a_member_ends_hazard_mode();
    test_hazards_restore_a_member_that_was_already_on();
    test_hazards_start_even_when_a_member_is_already_lit();
    test_restored_indicator_rearms_auto_cancel();

    test_default_config_mode_is_on();
    test_find_channel_by_role();
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
    test_starter_is_cut_when_engine_starts();
    test_engine_start_does_not_disturb_other_channels();
    test_engine_running_cut_is_edge_triggered();
    test_momentary_flag_is_inert_inside_the_engine();

    test_ignition_on_lights_its_companions();
    test_ignition_off_drops_its_companions();
    test_ignition_off_drops_every_channel_not_just_companions();
    test_ignition_off_ends_hazard_mode();
    test_ignition_off_sweep_is_edge_triggered();
    test_companion_switched_off_by_hand_stays_off();
    test_restore_does_not_fire_the_ignition_edge();
    test_immobilized_ignition_lights_no_companions();
    test_lighting_one_of_a_pair_puts_the_other_out();
    test_alternate_press_steps_between_members();
    test_direct_command_can_still_turn_a_pair_off();
    test_alternate_press_without_a_partner_is_rejected();
    test_alternate_press_still_obeys_the_starter_guards();
    test_config_validate_flags_asymmetric_pairs();
    test_config_validate_flags_pair_fighting_over_ignition();
    test_hazard_active_tracks_the_group();
    return 0;
}
