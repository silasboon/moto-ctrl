#include "mc_power.h"

#include <string.h>

#include "mc_types.h"

void mc_power_config_default(mc_power_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->idle_after_ms = MC_POWER_DEFAULT_IDLE_AFTER_MS;
    out->parked_after_ms = MC_POWER_DEFAULT_PARKED_AFTER_MS;
}

static void set_profile(mc_power_t *p, mc_power_state_t state)
{
    mc_power_profile_t next;
    switch (state) {
    case MC_POWER_PARKED:
        next.tick_interval_ms = MC_POWER_TICK_PARKED_MS;
        next.adv = MC_POWER_ADV_SLOW;
        next.light_sleep_allowed = true;
        break;
    case MC_POWER_IDLE:
        next.tick_interval_ms = MC_POWER_TICK_IDLE_MS;
        next.adv = MC_POWER_ADV_FAST;
        next.light_sleep_allowed = true;
        break;
    case MC_POWER_ACTIVE:
    default:
        next.tick_interval_ms = MC_POWER_TICK_ACTIVE_MS;
        next.adv = MC_POWER_ADV_FAST;
        next.light_sleep_allowed = false;
        break;
    }

    if (next.tick_interval_ms != p->profile.tick_interval_ms ||
        next.adv != p->profile.adv ||
        next.light_sleep_allowed != p->profile.light_sleep_allowed) {
        p->profile_changed = true;
    }
    p->profile = next;
    p->state = state;
}

void mc_power_init(mc_power_t *p, const mc_power_config_t *config, uint32_t now_ms)
{
    memset(p, 0, sizeof(*p));
    p->config = *config;
    p->last_active_ms = now_ms;
    /* Deliberately not via set_profile(): boot starts ACTIVE with no
     * pending change to apply, since the platform's own init has already
     * brought everything up at full rate. */
    p->state = MC_POWER_ACTIVE;
    p->profile.tick_interval_ms = MC_POWER_TICK_ACTIVE_MS;
    p->profile.adv = MC_POWER_ADV_FAST;
    p->profile.light_sleep_allowed = false;
    p->profile_changed = false;
}

void mc_power_tick(mc_power_t *p, const mc_power_inputs_t *in, uint32_t now_ms)
{
    /* Any hold-awake input pins ACTIVE and restarts the quiet timer. These
     * are the safety gates documented in mc_power.h, not tuning knobs: each
     * one is a case where idling down would break a behaviour the rider
     * depends on. */
    bool hold_awake = in->outputs_active ||
                      in->engine_running ||
                      in->ignition_live ||
                      in->ota_active ||
                      in->input_pending ||
                      in->factory_reset_armed;

    if (hold_awake) {
        p->last_active_ms = now_ms;
        set_profile(p, MC_POWER_ACTIVE);
        return;
    }

    /* A connected client never reaches PARKED: slow advertising would do
     * nothing for an established link, and the slowest tick would make the
     * app feel laggy for no saving worth having while someone is actually
     * using it. Light sleep is still allowed — the radio stays up. */
    if (in->session_connected) {
        set_profile(p, MC_POWER_IDLE);
        return;
    }

    if (mc_elapsed_at_least(now_ms, p->last_active_ms, p->config.parked_after_ms)) {
        set_profile(p, MC_POWER_PARKED);
    } else if (mc_elapsed_at_least(now_ms, p->last_active_ms, p->config.idle_after_ms)) {
        set_profile(p, MC_POWER_IDLE);
    } else {
        set_profile(p, MC_POWER_ACTIVE);
    }
}

bool mc_power_take_profile_change(mc_power_t *p)
{
    bool changed = p->profile_changed;
    p->profile_changed = false;
    return changed;
}
