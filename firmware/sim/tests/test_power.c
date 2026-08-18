/*
 * mc_power — the parked/idle power policy. The hold-awake gates are the
 * safety-relevant half of this module (idling down mid-gesture or with an
 * output driven is what they exist to prevent), so every gate gets its own
 * case, plus the full ACTIVE -> IDLE -> PARKED progression and the wake
 * path back.
 */
#include "mc_power.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static mc_power_inputs_t quiet_inputs(void)
{
    mc_power_inputs_t in;
    memset(&in, 0, sizeof(in));
    return in;
}

static void init_at(mc_power_t *p, uint32_t now_ms)
{
    mc_power_config_t cfg;
    mc_power_config_default(&cfg);
    mc_power_init(p, &cfg, now_ms);
}

static void test_boot_is_active_and_awake(void)
{
    mc_power_t p;
    init_at(&p, 0);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);
    assert(mc_power_get_profile(&p)->light_sleep_allowed == false);
    assert(mc_power_get_profile(&p)->tick_interval_ms == MC_POWER_TICK_ACTIVE_MS);
    assert(mc_power_get_profile(&p)->adv == MC_POWER_ADV_FAST);
    /* Nothing to apply yet — the platform already booted at full rate. */
    assert(mc_power_take_profile_change(&p) == false);
}

static void test_progresses_active_idle_parked(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();

    mc_power_tick(&p, &in, 100);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_IDLE_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_IDLE);
    assert(mc_power_get_profile(&p)->light_sleep_allowed == true);
    assert(mc_power_get_profile(&p)->tick_interval_ms == MC_POWER_TICK_IDLE_MS);
    /* Still discoverable quickly while only briefly idle. */
    assert(mc_power_get_profile(&p)->adv == MC_POWER_ADV_FAST);

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_PARKED);
    assert(mc_power_get_profile(&p)->light_sleep_allowed == true);
    assert(mc_power_get_profile(&p)->tick_interval_ms == MC_POWER_TICK_PARKED_MS);
    assert(mc_power_get_profile(&p)->adv == MC_POWER_ADV_SLOW);
}

/* Each hold-awake input, on its own, must pin ACTIVE and refuse sleep even
 * after long enough to have reached PARKED. */
static void test_each_gate_holds_awake(void)
{
    const struct {
        const char *name;
        size_t offset;
    } gates[] = {
        { "outputs_active", offsetof(mc_power_inputs_t, outputs_active) },
        { "engine_running", offsetof(mc_power_inputs_t, engine_running) },
        { "ignition_live", offsetof(mc_power_inputs_t, ignition_live) },
        { "ota_active", offsetof(mc_power_inputs_t, ota_active) },
        { "input_pending", offsetof(mc_power_inputs_t, input_pending) },
        { "factory_reset_armed", offsetof(mc_power_inputs_t, factory_reset_armed) },
    };

    for (size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
        mc_power_t p;
        init_at(&p, 0);
        mc_power_inputs_t in = quiet_inputs();
        *((bool *)((char *)&in + gates[i].offset)) = true;

        mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS * 10u);
        if (mc_power_get_state(&p) != MC_POWER_ACTIVE ||
            mc_power_get_profile(&p)->light_sleep_allowed ||
            mc_power_get_profile(&p)->tick_interval_ms != MC_POWER_TICK_ACTIVE_MS) {
            printf("test_power: gate '%s' failed to hold the loop awake\n", gates[i].name);
            assert(0);
        }
    }
}

/* A gate asserting mid-idle must snap straight back to ACTIVE — this is the
 * button-wake path: the GPIO interrupt runs the loop, input_pending is set,
 * and full-rate polling resumes before debounce needs it. */
static void test_gate_wakes_from_parked(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_PARKED);

    in.input_pending = true;
    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS + 10u);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);
    assert(mc_power_get_profile(&p)->light_sleep_allowed == false);
    assert(mc_power_get_profile(&p)->adv == MC_POWER_ADV_FAST);
}

/* Releasing a gate restarts the quiet timer rather than dropping straight
 * back to where it left off. */
static void test_quiet_timer_restarts_after_activity(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_PARKED);

    in.outputs_active = true;
    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS + 1000u);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);

    in.outputs_active = false;
    /* Only just quiet again: not yet even IDLE. */
    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS + 1100u);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);

    mc_power_tick(&p, &in,
                  MC_POWER_DEFAULT_PARKED_AFTER_MS + 1000u + MC_POWER_DEFAULT_IDLE_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_IDLE);
}

/* A connected client idles down but never reaches PARKED: slow advertising
 * does nothing for an established link, and the slowest tick would make the
 * app laggy while someone is actually using it. */
static void test_connected_session_never_parks(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();
    in.session_connected = true;

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS * 10u);
    assert(mc_power_get_state(&p) == MC_POWER_IDLE);
    assert(mc_power_get_profile(&p)->adv == MC_POWER_ADV_FAST);
    /* Still worth sleeping between ticks — the radio stays up. */
    assert(mc_power_get_profile(&p)->light_sleep_allowed == true);

    /* Disconnecting lets it park, timed from the last real activity. */
    in.session_connected = false;
    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS * 10u + 1u);
    assert(mc_power_get_state(&p) == MC_POWER_PARKED);
}

/* A hold-awake gate outranks a connected session. */
static void test_gate_outranks_session(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();
    in.session_connected = true;
    in.ota_active = true;

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS);
    assert(mc_power_get_state(&p) == MC_POWER_ACTIVE);
    assert(mc_power_get_profile(&p)->light_sleep_allowed == false);
}

/* The change flag is what gates touching the radio and the PM locks, so it
 * must latch exactly once per actual change. */
static void test_profile_change_flag_is_edge_triggered(void)
{
    mc_power_t p;
    init_at(&p, 0);
    mc_power_inputs_t in = quiet_inputs();

    mc_power_tick(&p, &in, 100);
    assert(mc_power_take_profile_change(&p) == false); /* still ACTIVE */

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_IDLE_AFTER_MS);
    assert(mc_power_take_profile_change(&p) == true);
    assert(mc_power_take_profile_change(&p) == false);

    /* Repeated ticks in the same state don't re-raise it. */
    mc_power_tick(&p, &in, MC_POWER_DEFAULT_IDLE_AFTER_MS + 100u);
    assert(mc_power_take_profile_change(&p) == false);

    mc_power_tick(&p, &in, MC_POWER_DEFAULT_PARKED_AFTER_MS);
    assert(mc_power_take_profile_change(&p) == true);
}

/* now_ms wrapping (49.7 days) must not strand the policy awake or asleep —
 * same unsigned-delta discipline the rest of the core uses. */
static void test_survives_millis_wrap(void)
{
    mc_power_t p;
    uint32_t base = 0xFFFFFF00u;
    init_at(&p, base);
    mc_power_inputs_t in = quiet_inputs();

    uint32_t after_wrap = base + MC_POWER_DEFAULT_PARKED_AFTER_MS; /* wraps */
    assert(after_wrap < base);
    mc_power_tick(&p, &in, after_wrap);
    assert(mc_power_get_state(&p) == MC_POWER_PARKED);
}

int main(void)
{
    test_boot_is_active_and_awake();
    test_progresses_active_idle_parked();
    test_each_gate_holds_awake();
    test_gate_wakes_from_parked();
    test_quiet_timer_restarts_after_activity();
    test_connected_session_never_parks();
    test_gate_outranks_session();
    test_profile_change_flag_is_edge_triggered();
    test_survives_millis_wrap();
    printf("test_power: all passed\n");
    return 0;
}
