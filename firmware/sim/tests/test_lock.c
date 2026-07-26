/*
 * mc_lock — the immobilizer state machine. This is the highest-scrutiny
 * area in the project (AGENTS.md: "extra test coverage — every state
 * transition"), so this file aims for full coverage of docs/PROTOCOL.md
 * §11's state diagram and truth table: every transition, every guard,
 * boot recovery from every persisted state, the cheat-code backoff policy,
 * and the wire-level dispatch of the COMMAND-channel opcodes.
 */
#include "mc_lock.h"

#include <assert.h>
#include <string.h>

#include "mc_session.h"

/* --- fixture: a lock + the output engine it immobilizes --- */

typedef struct {
    mc_output_engine_t output;
    mc_output_config_t out_cfg;
    mc_lock_t lock;
} lock_fixture_t;

static void hal_noop(uint8_t channel, bool on, void *ctx)
{
    (void)channel; (void)on; (void)ctx;
}

/* One ignition channel (5) and one starter channel (6) configured, unless
 * the caller overrides out_cfg before calling mc_output_init. */
static void fx_init_outputs(lock_fixture_t *fx)
{
    mc_output_config_default(&fx->out_cfg);
    fx->out_cfg.channels[5].function = MC_OUT_FUNC_IGNITION;
    fx->out_cfg.channels[6].function = MC_OUT_FUNC_STARTER;
    mc_output_hal_t hal = { .set = hal_noop, .ctx = NULL };
    mc_output_init(&fx->output, &fx->out_cfg, hal);
}

static mc_lock_inputs_t no_inputs(void)
{
    mc_lock_inputs_t in = { .ignition_switch_level = false, .engine_running = false };
    return in;
}

/* A cheat-code config: enabled, PHONE method, cheat-code {1,2,3,4} set. */
static void fx_enable_with_cheatcode(lock_fixture_t *fx, uint32_t grace_ms)
{
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    cfg.auto_lock_grace_ms = grace_ms;
    uint8_t code[4] = { 1, 2, 3, 4 };
    mc_lock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.config = cfg;
    assert(mc_lock_set_cheatcode(&tmp, code, 4, 0));
    tmp.config.immobilizer_enabled = true;
    tmp.config.methods_mask = MC_LOCK_METHOD_PHONE;
    mc_lock_init(&fx->lock, &tmp.config, false, &fx->output, 0);
    assert(fx->lock.state == MC_LOCK_ST_UNLOCKED);
}

static void enter_locked_via_grace(lock_fixture_t *fx, uint32_t *t)
{
    mc_lock_inputs_t in = no_inputs();
    mc_lock_tick(&fx->lock, &fx->output, *t, &in); /* UNLOCKED -> PARKED */
    assert(fx->lock.state == MC_LOCK_ST_PARKED);
    *t += fx->lock.config.auto_lock_grace_ms + 10;
    mc_lock_tick(&fx->lock, &fx->output, *t, &in); /* PARKED -> LOCKED */
    assert(fx->lock.state == MC_LOCK_ST_LOCKED);
}

static void press_code(lock_fixture_t *fx, const uint8_t *code, uint8_t len, uint32_t *t)
{
    for (uint8_t i = 0; i < len; i++) {
        mc_lock_cheatcode_press(&fx->lock, &fx->output, *t, code[i]);
        *t += 100;
    }
}

/* --- config validation --- */

static void test_config_validate(void)
{
    mc_output_config_t out;
    mc_output_config_default(&out);

    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    assert(mc_lock_config_validate(&cfg, &out) == MC_LOCK_CFG_OK); /* disabled: nothing to check */

    cfg.immobilizer_enabled = true;
    uint32_t flags = mc_lock_config_validate(&cfg, &out);
    assert(flags & MC_LOCK_CFG_ENABLE_REQUIRES_CHEATCODE);
    assert(flags & MC_LOCK_CFG_ENABLE_REQUIRES_IGNITION_CHANNEL); /* `out` has no ignition channel */

    cfg.cheatcode_set = true;
    cfg.cheatcode_len = 4;
    out.channels[5].function = MC_OUT_FUNC_IGNITION;
    assert(mc_lock_config_validate(&cfg, &out) == MC_LOCK_CFG_OK);

    cfg.methods_mask = MC_LOCK_METHOD_IGNITION_SWITCH;
    cfg.ignition_switch_input = -1;
    assert(mc_lock_config_validate(&cfg, &out) & MC_LOCK_CFG_BAD_IGNITION_SWITCH_INPUT);
    cfg.ignition_switch_input = 3;
    assert(mc_lock_config_validate(&cfg, &out) == MC_LOCK_CFG_OK);
}

/* --- boot restore (mc_lock_init) --- */

static void test_boot_disabled_config(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    mc_lock_init(&fx.lock, &cfg, true /* stale locked_flag, ignored: disabled */, &fx.output, 0);
    assert(fx.lock.state == MC_LOCK_ST_DISABLED);
    assert(fx.lock.locked_flag == false);
    assert(fx.output.immobilized == false);
}

static void test_boot_restores_locked_when_safe(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    uint32_t t = 0;
    enter_locked_via_grace(&fx, &t);
    assert(fx.lock.locked_flag == true);

    /* Simulate a reboot: fresh mc_lock_t, same persisted config+flag,
     * engine not running, ignition not live (force_off already ran when
     * we entered LOCKED). */
    mc_lock_t booted;
    mc_lock_init(&booted, &fx.lock.config, fx.lock.locked_flag, &fx.output, 100000);
    assert(booted.state == MC_LOCK_ST_LOCKED);
    assert(mc_lock_is_dirty(&booted) == false); /* no override needed */
    assert(fx.output.immobilized == true);
}

static void test_boot_ride_safe_override_ignition_live(void)
{
    /* AGENTS.md #1: never boot into LOCKED while ignition is live — a
     * brownout mid-ride must restore the bike as running. */
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    cfg.immobilizer_enabled = true;
    cfg.cheatcode_set = true;
    cfg.cheatcode_len = 4;

    /* Ignition channel commanded ON directly on the output config (as if
     * mc_output_restore_from_config() had just restored it, pre-brownout). */
    fx.output.config.channels[5].commanded_on = true;

    mc_lock_init(&fx.lock, &cfg, true /* persisted LOCKED */, &fx.output, 0);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED); /* override fired */
    assert(fx.lock.locked_flag == false);
    assert(mc_lock_is_dirty(&fx.lock) == true); /* corrected state should be persisted */
    assert(fx.output.immobilized == false);
}

static void test_boot_ride_safe_override_engine_running(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_output_set_engine_running(&fx.output, true);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    cfg.immobilizer_enabled = true;
    cfg.cheatcode_set = true;
    cfg.cheatcode_len = 4;

    mc_lock_init(&fx.lock, &cfg, true, &fx.output, 0);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
    assert(mc_lock_is_dirty(&fx.lock) == true);
}

/* --- DISABLED <-> UNLOCKED, auto-lock grace, PARKED guards --- */

static void test_disabled_to_unlocked_via_apply_config(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    mc_lock_init(&fx.lock, &cfg, false, &fx.output, 0);
    assert(fx.lock.state == MC_LOCK_ST_DISABLED);

    uint8_t code[4] = { 0, 1, 2, 3 };
    assert(mc_lock_set_cheatcode(&fx.lock, code, 4, 0));

    uint32_t flags = mc_lock_apply_config(&fx.lock, &fx.output, &fx.out_cfg,
                                          true, MC_LOCK_METHOD_PHONE, -1, 60000, 5000, 0);
    assert(flags == MC_LOCK_CFG_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
    assert(fx.lock.config.immobilizer_enabled == true);
}

static void test_apply_config_rejects_enable_without_cheatcode(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    mc_lock_init(&fx.lock, &cfg, false, &fx.output, 0);

    uint32_t flags = mc_lock_apply_config(&fx.lock, &fx.output, &fx.out_cfg,
                                          true, MC_LOCK_METHOD_PHONE, -1, 60000, 5000, 0);
    assert(flags & MC_LOCK_CFG_ENABLE_REQUIRES_CHEATCODE);
    assert(fx.lock.state == MC_LOCK_ST_DISABLED); /* unchanged */
    assert(fx.lock.config.immobilizer_enabled == false);
}

static void test_unlocked_to_parked_to_unlocked_on_engine_start(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);

    mc_lock_inputs_t in = no_inputs();
    mc_lock_tick(&fx.lock, &fx.output, 1000, &in);
    assert(fx.lock.state == MC_LOCK_ST_PARKED);

    in.engine_running = true;
    mc_lock_tick(&fx.lock, &fx.output, 2000, &in);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED); /* rider restarted; no re-auth needed */
}

static void test_unlocked_stays_unlocked_while_ignition_live(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.output.config.channels[5].commanded_on = true; /* ignition live */

    mc_lock_inputs_t in = no_inputs();
    mc_lock_tick(&fx.lock, &fx.output, 1000, &in);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED); /* parked_guard false: stays UNLOCKED */
}

static void test_parked_to_locked_via_grace_timer(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 1000);
    uint32_t t = 0;
    enter_locked_via_grace(&fx, &t);
    assert(fx.output.immobilized == true);
    assert(fx.lock.locked_flag == true);
    /* Ignition (and starter) must be forced off on entering LOCKED. */
    assert(mc_output_get_state(&fx.output, 5) == false);
}

/* --- explicit lock/unlock commands --- */

static void test_request_lock_idempotent_and_guarded(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);

    /* Guard: refuses while ignition is live. */
    fx.output.config.channels[5].commanded_on = true;
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_REJECTED);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);

    fx.output.config.channels[5].commanded_on = false;
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);

    /* Idempotent: locking an already-locked bike is OK, not an error. */
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
}

static void test_request_lock_unauthorized_when_disabled(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    mc_lock_init(&fx.lock, &cfg, false, &fx.output, 0);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_UNAUTHORIZED);
}

static void test_request_unlock_requires_phone_method_enabled(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.lock.config.methods_mask = 0; /* phone method disabled */
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);

    assert(mc_lock_request_unlock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_UNAUTHORIZED);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);

    fx.lock.config.methods_mask = MC_LOCK_METHOD_PHONE;
    assert(mc_lock_request_unlock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
    assert(fx.output.immobilized == false);
}

static void test_request_unlock_noop_when_not_locked(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_unlock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

/* --- passive auto-unlock: phone-as-key (edge-triggered), ignition-switch (level) --- */

/* Phone-as-key is edge-triggered — the platform calls mc_lock_request_unlock()
 * directly from mc_app_t.on_session_authed exactly once per fresh
 * authentication (mc_session.c), NOT via mc_lock_tick(). These tests call it
 * the same way the platform's hook does, standing in for "a phone just
 * authenticated". See mc_lock.h's doc comment on mc_lock_request_unlock()
 * for why: a level checked every tick would make an explicit MC_OP_LOCK
 * self-defeating while that same phone stays connected. */

static void test_phone_authed_edge_unlocks(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    assert(mc_lock_request_unlock(&fx.lock, &fx.output, 1000) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

static void test_phone_authed_edge_rejected_when_method_disabled(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.lock.config.methods_mask = 0;
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    assert(mc_lock_request_unlock(&fx.lock, &fx.output, 1000) == MC_LOCK_RESULT_UNAUTHORIZED);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED); /* a disabled method never overrides */
}

static void test_locking_while_phone_still_connected_stays_locked(void)
{
    /* The bug this design avoids: if phone-as-key were a level polled every
     * tick, locking the bike while that same phone's session is still
     * connected+authenticated would immediately re-unlock it on the very
     * next tick, making "lock now" self-defeating. With the edge-triggered
     * design, ticking after lock (with no fresh auth event) must NOT
     * unlock — the phone being "still" authenticated fires no new edge. */
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    mc_lock_inputs_t in = no_inputs();
    for (uint32_t t = 1000; t < 5000; t += 10) {
        mc_lock_tick(&fx.lock, &fx.output, t, &in);
    }
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
}

static void test_tick_auto_unlocks_on_ignition_switch(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.lock.config.methods_mask = MC_LOCK_METHOD_IGNITION_SWITCH;
    fx.lock.config.ignition_switch_input = 2;
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_REJECTED
           || mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    /* (methods_mask no longer includes PHONE, but request_lock only checks
     * immobilizer_enabled + the parked guard, not methods_mask.) */
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);

    mc_lock_inputs_t in = no_inputs();
    in.ignition_switch_level = true;
    mc_lock_tick(&fx.lock, &fx.output, 1000, &in);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

/* --- interrupted-transition / fault recovery --- */

static void test_simultaneous_unlock_triggers_are_idempotent(void)
{
    /* Phone auth (edge) and a completed correct cheat-code both arriving
     * "at once": no double-processing, no crash, ends UNLOCKED exactly
     * once regardless of which one a caller processes first. */
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.lock.config.methods_mask = MC_LOCK_METHOD_PHONE;
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 1000;
    uint8_t code[4] = { 1, 2, 3, 4 };
    assert(mc_lock_request_unlock(&fx.lock, &fx.output, t) == MC_LOCK_RESULT_OK); /* unlocks via phone first */
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);

    /* Cheat-code presses arriving after the fact are simply ignored (not
     * LOCKED anymore) — no error, no re-lock, no crash. */
    press_code(&fx, code, 4, &t);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);

    /* A second, redundant unlock (e.g. a duplicate/late auth edge) is a
     * harmless no-op — still just OK, still UNLOCKED. */
    assert(mc_lock_request_unlock(&fx.lock, &fx.output, t) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

/* --- cheat-code entry, backoff policy --- */

static void test_cheatcode_correct_entry_unlocks(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 1000;
    uint8_t code[4] = { 1, 2, 3, 4 };
    mc_lock_cheatcode_outcome_t last = MC_LOCK_CHEATCODE_PENDING;
    for (int i = 0; i < 4; i++) {
        last = mc_lock_cheatcode_press(&fx.lock, &fx.output, t, code[i]);
        t += 100;
    }
    assert(last == MC_LOCK_CHEATCODE_MATCH);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
    assert(fx.lock.consecutive_wrong == 0);
}

static void test_cheatcode_wrong_entry_does_not_unlock(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 1000;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    mc_lock_cheatcode_outcome_t last = MC_LOCK_CHEATCODE_PENDING;
    for (int i = 0; i < 4; i++) {
        last = mc_lock_cheatcode_press(&fx.lock, &fx.output, t, wrong[i]);
        t += 100;
    }
    assert(last == MC_LOCK_CHEATCODE_MISMATCH);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
    assert(fx.lock.consecutive_wrong == 1);
}

static void test_cheatcode_ignored_unless_locked(void)
{
    /* Presses while UNLOCKED/PARKED/DISABLED must never accumulate wrong
     * entries — ordinary riding (turn-signal/horn presses on these same 8
     * buttons) must never trigger backoff. */
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);

    uint32_t t = 1000;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    for (int i = 0; i < 4; i++) {
        mc_lock_cheatcode_outcome_t r = mc_lock_cheatcode_press(&fx.lock, &fx.output, t, wrong[i]);
        assert(r == MC_LOCK_CHEATCODE_PENDING); /* ignored: not LOCKED */
        t += 100;
    }
    assert(fx.lock.consecutive_wrong == 0);
}

static void test_cheatcode_timeout_not_counted_as_wrong(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    fx.lock.config.cheatcode_window_ms = 100; /* tight window */
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 0;
    mc_lock_cheatcode_press(&fx.lock, &fx.output, t, 1);
    t += 1000; /* well past the window */
    mc_lock_cheatcode_outcome_t r = mc_lock_cheatcode_press(&fx.lock, &fx.output, t, 2);
    assert(r == MC_LOCK_CHEATCODE_PENDING); /* buffer reset, this is a fresh first press */
    assert(fx.lock.consecutive_wrong == 0);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
}

static void test_cheatcode_backoff_after_five_free_attempts(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 0;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    for (int attempt = 1; attempt <= 5; attempt++) {
        press_code(&fx, wrong, 4, &t);
        assert(fx.lock.consecutive_wrong == (uint16_t)attempt);
        assert(fx.lock.backoff_active == false); /* first 5 are free */
    }

    /* 6th wrong entry: backoff kicks in. */
    press_code(&fx, wrong, 4, &t);
    assert(fx.lock.consecutive_wrong == 6);
    assert(fx.lock.backoff_active == true);

    /* While in backoff, presses (even the correct code) are ignored. */
    uint8_t code[4] = { 1, 2, 3, 4 };
    mc_lock_cheatcode_outcome_t r = mc_lock_cheatcode_press(&fx.lock, &fx.output, t, code[0]);
    assert(r == MC_LOCK_CHEATCODE_IN_BACKOFF);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
}

static void test_cheatcode_backoff_expires_and_unlock_still_works(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 0;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    for (int attempt = 1; attempt <= 6; attempt++) {
        press_code(&fx, wrong, 4, &t);
    }
    assert(fx.lock.backoff_active == true);

    /* Advance past the 15s (6th-attempt) backoff duration; a tick clears it. */
    t += 16000;
    mc_lock_inputs_t in = no_inputs();
    mc_lock_tick(&fx.lock, &fx.output, t, &in);
    assert(fx.lock.backoff_active == false);

    /* And — crucially — the rider is never locked out forever: the phone
     * and ignition-switch methods were never gated by the cheat-code
     * backoff at all (AGENTS.md #3). Demonstrate via request_unlock. */
    fx.lock.config.methods_mask = MC_LOCK_METHOD_PHONE;
    assert(mc_lock_request_unlock(&fx.lock, &fx.output, t) == MC_LOCK_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

static void test_cheatcode_quiet_period_forgives_wrong_count(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 0;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    press_code(&fx, wrong, 4, &t);
    assert(fx.lock.consecutive_wrong == 1);

    /* 5+ minutes of no further attempts forgives the counter. */
    t += 6u * 60u * 1000u;
    mc_lock_inputs_t in = no_inputs();
    mc_lock_tick(&fx.lock, &fx.output, t, &in);
    assert(fx.lock.consecutive_wrong == 0);
}

static void test_cheatcode_unlock_resets_backoff_state(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint32_t t = 0;
    uint8_t wrong[4] = { 4, 3, 2, 1 };
    press_code(&fx, wrong, 4, &t);
    press_code(&fx, wrong, 4, &t);
    assert(fx.lock.consecutive_wrong == 2);

    uint8_t code[4] = { 1, 2, 3, 4 };
    press_code(&fx, code, 4, &t);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
    assert(fx.lock.consecutive_wrong == 0);
    assert(fx.lock.backoff_active == false);
}

/* --- cheat-code set/clear/test --- */

static void test_set_cheatcode_validates_length_and_buttons(void)
{
    mc_lock_t lock;
    memset(&lock, 0, sizeof(lock));
    mc_lock_config_default(&lock.config);

    uint8_t too_short[3] = { 0, 1, 2 };
    assert(mc_lock_set_cheatcode(&lock, too_short, 3, 0) == false);

    uint8_t too_long[11] = {0,1,2,3,4,5,6,7,0,1,2};
    assert(mc_lock_set_cheatcode(&lock, too_long, 11, 0) == false);

    uint8_t bad_button[4] = { 0, 1, 8, 2 }; /* 8 is out of range (0..7) */
    assert(mc_lock_set_cheatcode(&lock, bad_button, 4, 0) == false);

    uint8_t ok[4] = { 0, 1, 2, 3 };
    assert(mc_lock_set_cheatcode(&lock, ok, 4, 0) == true);
    assert(lock.config.cheatcode_set == true);
    assert(lock.config.cheatcode_len == 4);
}

static void test_set_cheatcode_salts_differently_each_time(void)
{
    mc_lock_t lock;
    memset(&lock, 0, sizeof(lock));
    mc_lock_config_default(&lock.config);
    uint8_t code[4] = { 0, 1, 2, 3 };

    assert(mc_lock_set_cheatcode(&lock, code, 4, 0));
    uint8_t salt1[MC_LOCK_SALT_BYTES];
    memcpy(salt1, lock.config.cheatcode_salt, sizeof(salt1));

    assert(mc_lock_set_cheatcode(&lock, code, 4, 0));
    /* Astronomically unlikely to collide with a real CSPRNG; if this ever
     * flakes, the RNG is broken, not the test. */
    assert(memcmp(salt1, lock.config.cheatcode_salt, sizeof(salt1)) != 0);
    assert(mc_lock_test_cheatcode(&lock.config, code, 4) == true);
}

static void test_clear_cheatcode_blocked_while_enabled(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);

    assert(mc_lock_clear_cheatcode(&fx.lock, 0) == false); /* mandatory fallback while enabled */
    assert(fx.lock.config.cheatcode_set == true);

    fx.lock.config.immobilizer_enabled = false;
    assert(mc_lock_clear_cheatcode(&fx.lock, 0) == true);
    assert(fx.lock.config.cheatcode_set == false);
}

static void test_test_cheatcode_is_pure(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    uint8_t code[4] = { 1, 2, 3, 4 };
    uint8_t wrong[4] = { 1, 1, 1, 1 };
    assert(mc_lock_test_cheatcode(&fx.lock.config, code, 4) == true);
    assert(mc_lock_test_cheatcode(&fx.lock.config, wrong, 4) == false);

    /* Practice mode touches nothing: still LOCKED, no wrong-entry counted. */
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
    assert(fx.lock.consecutive_wrong == 0);
    assert(fx.lock.entry_len == 0);
}

/* --- ownership transfer / factory reset --- */

static void test_transfer_ownership_resets_everything(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 60000);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);
    assert(fx.output.immobilized == true);

    mc_lock_transfer_ownership(&fx.lock, &fx.output, 0);
    assert(fx.lock.state == MC_LOCK_ST_DISABLED);
    assert(fx.lock.config.immobilizer_enabled == false);
    assert(fx.lock.config.cheatcode_set == false);
    assert(fx.lock.locked_flag == false);
    assert(fx.output.immobilized == false); /* never strand the next owner */
}

/* --- output engine immobilize enforcement --- */

static void test_output_blocks_ignition_and_starter_while_immobilized(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    cfg.channels[5].function = MC_OUT_FUNC_IGNITION;
    cfg.channels[6].function = MC_OUT_FUNC_STARTER;
    mc_output_hal_t hal = { .set = hal_noop, .ctx = NULL };
    mc_output_engine_t eng;
    mc_output_init(&eng, &cfg, hal);
    mc_output_set_immobilized(&eng, true);

    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_REMOTE) == MC_OUT_ERR_IMMOBILIZED);
    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_ERR_IMMOBILIZED);
    assert(mc_output_set(&eng, 6, true, MC_OUT_SRC_LOCAL) == MC_OUT_ERR_IMMOBILIZED);
    assert(mc_output_get_state(&eng, 5) == false);

    /* Turning OFF is never blocked, even while immobilized. */
    assert(mc_output_set(&eng, 5, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);

    /* A non-ignition/starter channel is unaffected. */
    cfg.channels[0].function = MC_OUT_FUNC_HORN;
    eng.config.channels[0].function = MC_OUT_FUNC_HORN;
    assert(mc_output_set(&eng, 0, true, MC_OUT_SRC_REMOTE) == MC_OUT_OK);

    mc_output_set_immobilized(&eng, false);
    assert(mc_output_set(&eng, 5, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
}

static void test_find_ignition_channel(void)
{
    mc_output_config_t cfg;
    mc_output_config_default(&cfg);
    assert(mc_output_find_ignition_channel(&cfg) == -1);
    cfg.channels[8].function = MC_OUT_FUNC_IGNITION;
    assert(mc_output_find_ignition_channel(&cfg) == 8);
}

/* --- persistence round-trip --- */

static void test_serialize_deserialize_roundtrip(void)
{
    lock_fixture_t fx;
    fx_init_outputs(&fx);
    fx_enable_with_cheatcode(&fx, 45000);
    fx.lock.locked_flag = true;

    uint8_t buf[512];
    size_t len = 0;
    assert(mc_lock_serialize(&fx.lock, buf, sizeof(buf), &len) == MC_LOCK_STORE_OK);

    mc_lock_config_t out_cfg;
    bool out_locked = false;
    assert(mc_lock_deserialize(buf, len, &out_cfg, &out_locked) == MC_LOCK_STORE_OK);
    assert(out_locked == true);
    assert(out_cfg.immobilizer_enabled == true);
    assert(out_cfg.auto_lock_grace_ms == 45000);
    assert(out_cfg.cheatcode_set == true);
    assert(memcmp(out_cfg.cheatcode_hash, fx.lock.config.cheatcode_hash, MC_CRYPTO_HASH_BYTES) == 0);

    /* Corrupt buffer -> corrupt result, never crashes. */
    buf[0] ^= 0xFF;
    assert(mc_lock_deserialize(buf, len, &out_cfg, &out_locked) == MC_LOCK_STORE_ERR_CORRUPT);
}

/* --- wire-level (mc_session) dispatch of the new opcodes --- */

typedef struct {
    mc_channel_t ch;
    uint8_t data[64];
    size_t len;
} rec_frame_t;

typedef struct {
    rec_frame_t frames[32];
    int count;
} recorder_t;

static void rec_send(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    recorder_t *r = (recorder_t *)io;
    assert(r->count < 32);
    assert(len <= sizeof(r->frames[0].data));
    r->frames[r->count].ch = ch;
    memcpy(r->frames[r->count].data, data, len);
    r->frames[r->count].len = len;
    r->count++;
}

static void rec_reset(recorder_t *r) { r->count = 0; }

static const rec_frame_t *last_frame(const recorder_t *r, mc_channel_t ch, uint8_t opcode)
{
    for (int i = r->count - 1; i >= 0; i--) {
        if (r->frames[i].ch == ch && r->frames[i].len >= 1 && r->frames[i].data[0] == opcode) {
            return &r->frames[i];
        }
    }
    return NULL;
}

typedef struct {
    mc_output_engine_t output;
    mc_config_t config;
    mc_keystore_t keystore;
    mc_lock_t lock;
    mc_app_t app;
    int persist_lock_calls;
} session_fixture_t;

static void sfx_persist_lock(void *ctx) { ((session_fixture_t *)ctx)->persist_lock_calls++; }

static void sfx_init(session_fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    mc_config_default(&fx->config);
    fx->config.outputs.channels[5].function = MC_OUT_FUNC_IGNITION;
    mc_keystore_init(&fx->keystore);
    mc_output_hal_t hal = { .set = NULL, .ctx = NULL };
    mc_output_init(&fx->output, &fx->config.outputs, hal);

    mc_lock_config_t lcfg;
    mc_lock_config_default(&lcfg);
    mc_lock_init(&fx->lock, &lcfg, false, &fx->output, 0);

    fx->app.output = &fx->output;
    fx->app.config = &fx->config;
    fx->app.keystore = &fx->keystore;
    fx->app.lock = &fx->lock;
    fx->app.persist_lock = sfx_persist_lock;
    fx->app.app_ctx = fx;
}

/* Enrolls (TOFU) + authenticates a fresh session; returns the slot. */
static int sfx_auth(session_fixture_t *fx, mc_session_t *s, recorder_t *rec)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    int slot = mc_keystore_add(&fx->keystore, pk, "Phone");

    mc_session_init(s);
    uint8_t begin = MC_OP_AUTH_BEGIN;
    rec_reset(rec);
    mc_session_handle(s, &fx->app, MC_CH_AUTH, &begin, 1, rec_send, rec);
    const rec_frame_t *chal = last_frame(rec, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE);
    assert(chal != NULL);

    uint8_t msg[MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES];
    size_t msg_len = mc_session_build_auth_message(chal->data + 1, msg);
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_sign(sig, msg, msg_len, sk));

    uint8_t resp[1 + MC_CRYPTO_SIG_BYTES];
    resp[0] = MC_OP_AUTH_RESPONSE;
    memcpy(resp + 1, sig, MC_CRYPTO_SIG_BYTES);
    rec_reset(rec);
    mc_session_handle(s, &fx->app, MC_CH_AUTH, resp, sizeof(resp), rec_send, rec);
    assert(mc_session_is_authed(s));
    return slot;
}

static void test_wire_cheatcode_set_lock_unlock_flow(void)
{
    session_fixture_t fx;
    sfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    sfx_auth(&fx, &s, &rec);

    /* Unauthenticated session can't touch any lock opcode. */
    mc_session_t unauth;
    mc_session_init(&unauth);
    rec_reset(&rec);
    uint8_t lock_op = MC_OP_LOCK;
    mc_session_handle(&unauth, &fx.app, MC_CH_COMMAND, &lock_op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_UNAUTHENTICATED);

    /* Set a cheat-code over the wire. */
    rec_reset(&rec);
    uint8_t set_cmd[6] = { MC_OP_CHEATCODE_SET, 4, 1, 2, 3, 4 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, set_cmd, sizeof(set_cmd), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[1] == MC_OP_CHEATCODE_SET && r->data[2] == MC_RESULT_OK);
    assert(fx.persist_lock_calls == 1);

    /* Enable the immobilizer via LOCK_SET_CONFIG. */
    rec_reset(&rec);
    uint8_t set_cfg[8] = { MC_OP_LOCK_SET_CONFIG, 1 /* enabled */, MC_LOCK_METHOD_PHONE, 0xFF, 0x60, 0xEA, 0x88, 0x13 };
    /* auto_lock_grace_ms = 0xEA60 (60000), cheatcode_window_ms = 0x1388 (5000) */
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, set_cfg, sizeof(set_cfg), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[1] == MC_OP_LOCK_SET_CONFIG && r->data[2] == MC_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);

    /* Read it back. */
    rec_reset(&rec);
    uint8_t get_cfg = MC_OP_LOCK_GET_CONFIG;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &get_cfg, 1, rec_send, &rec);
    const rec_frame_t *cfgr = last_frame(&rec, MC_CH_COMMAND, MC_OP_LOCK_CONFIG);
    assert(cfgr != NULL && cfgr->len == 1 + 9);
    assert(cfgr->data[1] == 1); /* enabled */
    assert(cfgr->data[8] == 1); /* cheatcode_set (resp[7], offset by the leading opcode byte) */
    assert(cfgr->data[9] == 4); /* cheatcode_len (resp[8]) */

    /* Lock (guard holds: ignition channel exists and is off). */
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &lock_op, 1, rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);

    /* Practice cheat-code test: wrong guess reported, no side effects. */
    rec_reset(&rec);
    uint8_t test_cmd[6] = { MC_OP_CHEATCODE_TEST, 4, 9, 9, 9, 9 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, test_cmd, sizeof(test_cmd), rec_send, &rec);
    const rec_frame_t *tr = last_frame(&rec, MC_CH_COMMAND, MC_OP_CHEATCODE_TEST_RESULT);
    assert(tr != NULL && tr->data[1] == MC_RESULT_OK && tr->data[2] == 0 /* no match */);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED); /* untouched */

    /* Explicit unlock. */
    rec_reset(&rec);
    uint8_t unlock_op = MC_OP_UNLOCK;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &unlock_op, 1, rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_UNLOCKED);
}

static void test_wire_transfer_ownership_wipes_keys_and_lock(void)
{
    session_fixture_t fx;
    sfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    sfx_auth(&fx, &s, &rec);
    assert(mc_keystore_count(&fx.keystore) == 1);

    uint8_t code[4] = { 0, 1, 2, 3 };
    assert(mc_lock_set_cheatcode(&fx.lock, code, 4, 0));
    fx.lock.config.immobilizer_enabled = true;

    rec_reset(&rec);
    uint8_t xfer = MC_OP_TRANSFER_OWNERSHIP;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &xfer, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);

    assert(mc_keystore_count(&fx.keystore) == 0);
    assert(fx.lock.config.immobilizer_enabled == false);
    assert(fx.lock.config.cheatcode_set == false);
}

static void test_wire_status_reports_live_lock_state(void)
{
    session_fixture_t fx;
    sfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    sfx_auth(&fx, &s, &rec);

    uint8_t code[4] = { 1, 2, 3, 4 };
    assert(mc_lock_set_cheatcode(&fx.lock, code, 4, 0));
    uint32_t flags = mc_lock_apply_config(&fx.lock, &fx.output, &fx.config.outputs,
                                          true, MC_LOCK_METHOD_PHONE, -1, 60000, 5000, 0);
    assert(flags == MC_LOCK_CFG_OK);
    assert(mc_lock_request_lock(&fx.lock, &fx.output, 0) == MC_LOCK_RESULT_OK);

    rec_reset(&rec);
    uint8_t get = MC_OP_STATUS_GET;
    mc_session_handle(&s, &fx.app, MC_CH_STATUS, &get, 1, rec_send, &rec);
    const rec_frame_t *st = last_frame(&rec, MC_CH_STATUS, MC_OP_STATUS);
    assert(st != NULL && st->len == 1 + MC_STATUS_WIRE_LEN);
    assert(st->data[1 + 3] == MC_LOCK_LOCKED); /* byte 3 of the status payload */
}

int main(void)
{
    test_config_validate();

    test_boot_disabled_config();
    test_boot_restores_locked_when_safe();
    test_boot_ride_safe_override_ignition_live();
    test_boot_ride_safe_override_engine_running();

    test_disabled_to_unlocked_via_apply_config();
    test_apply_config_rejects_enable_without_cheatcode();
    test_unlocked_to_parked_to_unlocked_on_engine_start();
    test_unlocked_stays_unlocked_while_ignition_live();
    test_parked_to_locked_via_grace_timer();

    test_request_lock_idempotent_and_guarded();
    test_request_lock_unauthorized_when_disabled();
    test_request_unlock_requires_phone_method_enabled();
    test_request_unlock_noop_when_not_locked();

    test_phone_authed_edge_unlocks();
    test_phone_authed_edge_rejected_when_method_disabled();
    test_locking_while_phone_still_connected_stays_locked();
    test_tick_auto_unlocks_on_ignition_switch();

    test_simultaneous_unlock_triggers_are_idempotent();

    test_cheatcode_correct_entry_unlocks();
    test_cheatcode_wrong_entry_does_not_unlock();
    test_cheatcode_ignored_unless_locked();
    test_cheatcode_timeout_not_counted_as_wrong();
    test_cheatcode_backoff_after_five_free_attempts();
    test_cheatcode_backoff_expires_and_unlock_still_works();
    test_cheatcode_quiet_period_forgives_wrong_count();
    test_cheatcode_unlock_resets_backoff_state();

    test_set_cheatcode_validates_length_and_buttons();
    test_set_cheatcode_salts_differently_each_time();
    test_clear_cheatcode_blocked_while_enabled();
    test_test_cheatcode_is_pure();

    test_transfer_ownership_resets_everything();

    test_output_blocks_ignition_and_starter_while_immobilized();
    test_find_ignition_channel();

    test_serialize_deserialize_roundtrip();

    test_wire_cheatcode_set_lock_unlock_flow();
    test_wire_transfer_ownership_wipes_keys_and_lock();
    test_wire_status_reports_live_lock_state();

    return 0;
}
