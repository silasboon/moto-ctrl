#include "mc_output.h"

#include <string.h>

void mc_output_config_default(mc_output_config_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        /* TOGGLE, and every role flag clear: a fresh channel is an
         * unnamed, latching, non-essential output with no special powers
         * until the rider gives it some. */
        out->channels[i].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
        out->channels[i].essential = false;
        out->channels[i].is_ignition = false;
        out->channels[i].is_starter = false;
        out->channels[i].is_brake = false;
        out->channels[i].indicator = MC_INDICATOR_NONE;
        out->channels[i].hazard_member = false;
        out->channels[i].on_with_ignition = false;
        out->channels[i].alternate_channel = -1;
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
/* The on-half of the blink cycle at `now_ms`. Shared by the BLINK behaviour
 * and by hazard mode, so a DRL joining the hazards flashes in phase with the
 * indicators rather than on its own schedule. */
static bool blink_phase(const mc_output_engine_t *eng, uint32_t now_ms)
{
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

static bool compute_driven_on(const mc_output_engine_t *eng, uint8_t channel, uint32_t now_ms)
{
    const mc_output_channel_config_t *ch = &eng->config.channels[channel];
    if (!ch->commanded_on) {
        return false;
    }
    /* Hazards override a member's own behaviour for as long as they are
     * running. Membership decides WHICH channels join; it must also decide
     * HOW they behave while joined, or a solid-by-nature channel (a DRL, an
     * aux light) sits steady while the indicators flash beside it — which is
     * not "blinking with the hazards" in any useful sense.
     *
     * Deliberately not solved by asking the rider to set the channel's
     * behaviour to BLINK: that would make the DRL blink during ordinary
     * running-light use too. Normal behaviour and hazard behaviour are
     * different questions, so hazard mode answers the second one itself. */
    if (eng->hazard_active && ch->hazard_member) {
        return blink_phase(eng, now_ms);
    }

    switch (ch->behaviour) {
    /* MOMENTARY is driven purely by commanded_on: the platform tick loop
     * turns it on and off from the trigger's level, so by the time we get
     * here it is an ordinary on/off channel. */
    case MC_OUT_BEHAVIOUR_TOGGLE:
    case MC_OUT_BEHAVIOUR_MOMENTARY:
        return true;
    case MC_OUT_BEHAVIOUR_BLINK:
        return blink_phase(eng, now_ms);
    case MC_OUT_BEHAVIOUR_FLASHER: {
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
    /* Dimming composes with any behaviour, but never with a pattern: BLINK
     * and FLASHER are full on/off by the PWM/flasher rule. */
    bool dimmed = ch->pwm_duty_pct < 100 &&
                  (ch->behaviour == MC_OUT_BEHAVIOUR_TOGGLE ||
                   ch->behaviour == MC_OUT_BEHAVIOUR_MOMENTARY);
    if (driven_on && dimmed && eng->hal.set_duty != NULL) {
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
    if (!was_on && on && ch->behaviour == MC_OUT_BEHAVIOUR_FLASHER) {
        eng->brake_burst_started_ms[channel] = now_ms;
    }
    apply_hal_state(eng, channel, now_ms);
}

/* Puts every hazard member back where it was before the hazards started, and
 * clears hazard mode.
 *
 * `except_channel` (-1 for none) is left alone: when the rider ends the
 * hazards by commanding a member directly — signalling a turn out of a hazard
 * stop — that channel is about to be set by the caller, and restoring it
 * first would fight the command.
 *
 * Restoring is the whole point. Before this, ending the hazards forced every
 * member off, so a DRL that had been lit beforehand went dark, and the
 * opposite indicator stayed latched on and blinking. */
static void end_hazard_mode(mc_output_engine_t *eng, int except_channel, uint32_t now_ms)
{
    if (!eng->hazard_active) {
        return;
    }
    eng->hazard_active = false;

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (!eng->config.channels[ch].hazard_member || (int)ch == except_channel) {
            continue;
        }
        bool prior = ((eng->hazard_saved_mask >> ch) & 1u) != 0;
        if (eng->config.channels[ch].commanded_on != prior) {
            set_commanded_on_raw(eng, ch, prior, now_ms);
        }
        /* An indicator restored to ON was mid-signal when the hazards
         * interrupted it, and its auto-cancel deadline died with the
         * interruption. Re-arm from now rather than leaving a signal latched
         * on for the rest of the ride. */
        eng->turn_auto_cancel_deadline_ms[ch] =
            (prior && eng->config.channels[ch].indicator != MC_INDICATOR_NONE &&
             eng->config.turn_auto_cancel_ms > 0)
                ? now_ms + eng->config.turn_auto_cancel_ms
                : 0;
    }
}

/* Key turned to "on": light the channels flagged to come up with it.
 *
 * Raw, so this can't recurse back through mc_output_set() into the ignition
 * handling that called it, and so a companion that happens to be an
 * indicator or an alternating-pair member doesn't drag mutual-exclusion
 * policy into what is meant to be a simple "these come up with the key".
 * Config validation already refuses the one combination that would be
 * ambiguous — both members of a pair flagged on_with_ignition. */
static void apply_ignition_on(mc_output_engine_t *eng, uint32_t now_ms)
{
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        const mc_output_channel_config_t *cfg = &eng->config.channels[ch];
        if (!cfg->on_with_ignition || cfg->is_ignition || cfg->commanded_on) {
            continue;
        }
        set_commanded_on_raw(eng, ch, true, now_ms);
        /* A companion that was mid-turn-signal shouldn't keep a stale
         * auto-cancel deadline pointed at it. */
        eng->turn_auto_cancel_deadline_ms[ch] = 0;
    }
}

/* Key turned off: EVERY channel goes out, not only the ones flagged
 * on_with_ignition.
 *
 * The flag says what comes on with the key; it was never meant to be the list
 * of what goes off with it. Keying the off-sweep to it left anything the
 * rider had switched on by hand — a headlight, an aux circuit — burning after
 * the ignition was killed, which is not what any key on any bike does and is
 * a good way to find a flat battery.
 *
 * This is a deliberate rider command with the bike stopped, not an error
 * path, so dropping the headlight here is correct and does not touch
 * ride-safe failure (which protects those channels while the bike is RUNNING).
 *
 * Hazard mode ends with it: leaving the flag set while every member is dark
 * would report running hazards on the status wire and make the next hazard
 * press read as "stop" when nothing is going. */
static void apply_ignition_off(mc_output_engine_t *eng, uint32_t now_ms)
{
    eng->hazard_active = false;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        /* The ignition channel itself is being set by the caller. */
        if (eng->config.channels[ch].is_ignition) {
            continue;
        }
        if (eng->config.channels[ch].commanded_on) {
            set_commanded_on_raw(eng, ch, false, now_ms);
        }
        eng->turn_auto_cancel_deadline_ms[ch] = 0;
    }
}

mc_output_result_t mc_output_set(mc_output_engine_t *eng, uint8_t channel, bool on, mc_output_source_t source)
{
    if (channel >= MC_OUTPUT_COUNT) {
        return MC_OUT_ERR_BAD_CHANNEL;
    }

    mc_output_channel_config_t *ch_cfg = &eng->config.channels[channel];

    /* A locked bike switches nothing on. Not just ignition and starter — a
     * passerby who can flick the headlight or blow the horn on an immobilized
     * bike is being handed a toy, and on a shared street it reads as the
     * alarm going off for no reason.
     *
     * Switching OFF is always allowed: nothing about the immobilizer should
     * make it harder to make the bike dark and quiet.
     *
     * Hazards are the one exception, and they don't come through here —
     * mc_output_hazard_press() drives its group with set_commanded_on_raw()
     * and is deliberately not gated, so a broken-down bike can still be left
     * flashing at the roadside while locked. Commanding a hazard MEMBER
     * directly is still refused; only the hazard control itself is exempt. */
    if (on && eng->immobilized) {
        return MC_OUT_ERR_IMMOBILIZED;
    }

    if (on && ch_cfg->is_starter) {
        mc_output_result_t err = validate_starter_command(eng, source);
        if (err != MC_OUT_OK) {
            return err;
        }
    }

    /* Any deliberate command to a group member ends hazard mode — signalling a
     * turn, or switching the DRL on by itself, means the rider is no longer
     * running hazards. Without this the flag could outlive the hazards and
     * make a later, unrelated switch-on of that channel blink.
     *
     * The other members go back to their pre-hazard state as they would on a
     * second hazard press; this channel is skipped because it is about to be
     * set explicitly, just below. */
    if (ch_cfg->hazard_member) {
        end_hazard_mode(eng, (int)channel, eng->last_tick_ms);
    }

    bool is_turn = (ch_cfg->indicator != MC_INDICATOR_NONE);

    if (is_turn && on) {
        mc_indicator_side_t opposite_side =
            (ch_cfg->indicator == MC_INDICATOR_LEFT) ? MC_INDICATOR_RIGHT : MC_INDICATOR_LEFT;
        int opposite = mc_output_find_indicator_channel(&eng->config, opposite_side);
        if (opposite >= 0 && opposite != channel && eng->config.channels[opposite].commanded_on) {
            set_commanded_on_raw(eng, (uint8_t)opposite, false, eng->last_tick_ms);
            eng->turn_auto_cancel_deadline_ms[opposite] = 0;
        }
    }

    /* Alternating pair (hi/lo beam, two DRL colours): lighting one puts the
     * other out. Enforced here rather than in the press helper so it holds
     * for every caller — an app tap on "High Beam" drops the low beam just
     * as a handlebar button does. */
    if (on) {
        int8_t partner = ch_cfg->alternate_channel;
        if (partner >= 0 && partner < MC_OUTPUT_COUNT && partner != (int8_t)channel &&
            eng->config.channels[partner].commanded_on) {
            set_commanded_on_raw(eng, (uint8_t)partner, false, eng->last_tick_ms);
        }
    }

    bool was_on = ch_cfg->commanded_on;
    set_commanded_on_raw(eng, channel, on, eng->last_tick_ms);

    if (is_turn) {
        eng->turn_auto_cancel_deadline_ms[channel] =
            (on && eng->config.turn_auto_cancel_ms > 0) ? eng->last_tick_ms + eng->config.turn_auto_cancel_ms : 0;
    }

    /* Ignition edge, after the channel itself has settled. Only a genuine
     * transition counts — re-commanding an already-on ignition must not
     * re-light a companion the rider deliberately switched off. */
    if (ch_cfg->is_ignition && was_on != on) {
        if (on) {
            apply_ignition_on(eng, eng->last_tick_ms);
        } else {
            apply_ignition_off(eng, eng->last_tick_ms);
        }
    }

    return MC_OUT_OK;
}

mc_output_result_t mc_output_alternate_press(mc_output_engine_t *eng, uint8_t channel,
                                             mc_output_source_t source)
{
    if (channel >= MC_OUTPUT_COUNT) {
        return MC_OUT_ERR_BAD_CHANNEL;
    }
    int8_t partner = eng->config.channels[channel].alternate_channel;
    if (partner < 0 || partner >= MC_OUTPUT_COUNT || partner == (int8_t)channel) {
        return MC_OUT_ERR_BAD_CHANNEL;
    }

    /* Whichever member is lit, light the other; from cold, light `channel`.
     * mc_output_set()'s pair exclusion puts the outgoing one out, so this
     * never has to command an OFF — which is exactly why the cycle can't
     * land on dark. */
    bool self_on = eng->config.channels[channel].commanded_on;
    uint8_t next = self_on ? (uint8_t)partner : channel;
    return mc_output_set(eng, next, true, source);
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
    if (eng->lv_cutoff && !mc_output_channel_is_essential(&eng->config.channels[channel])) {
        return false;
    }
    return compute_driven_on(eng, channel, now_ms);
}

void mc_output_hazard_press(mc_output_engine_t *eng, uint32_t now_ms)
{
    /* The hazard group is whatever the rider marked hazard_member — normally
     * both indicators, but a DRL or aux light can join, which is why this is
     * no longer hardcoded to the two turn channels. */
    int members = 0;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (eng->config.channels[ch].hazard_member) {
            members++;
        }
    }
    if (members == 0) {
        return;
    }

    /* Keyed on hazard mode itself, not on "is every member lit". The two used
     * to be the same thing only because ending the hazards forced the whole
     * group off; now that members are restored, a group can legitimately have
     * some channels on while the hazards are stopped (a lit DRL), and that
     * must still read as "press starts the hazards". */
    if (eng->hazard_active) {
        end_hazard_mode(eng, -1, now_ms);
        return;
    }

    /* Remember what to come back to, then take the group over. */
    eng->hazard_saved_mask = 0;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (!eng->config.channels[ch].hazard_member) {
            continue;
        }
        if (eng->config.channels[ch].commanded_on) {
            eng->hazard_saved_mask |= (uint16_t)(1u << ch);
        }
        /* Raw: hazards deliberately bypass turn mutual exclusion (both sides
         * on together is the whole point) and arm no auto-cancel — they stay
         * on until pressed again, like a real hazard switch. */
        set_commanded_on_raw(eng, ch, true, now_ms);
        eng->turn_auto_cancel_deadline_ms[ch] = 0;
    }
    eng->hazard_active = true;
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
    bool was_running = eng->engine_running;
    eng->engine_running = running;

    /* starter protection: the starter is inhibited while the engine is running.
     * validate_starter_command() only enforces that at COMMAND time, which
     * leaves the case that actually matters — the starter is already
     * engaged and the engine catches. A cranking motor left engaged against
     * a running engine destroys itself, and the rider is mid-start with the
     * button held, so nothing else is going to drop it. Cut it here, on the
     * edge, regardless of who commanded it or how it is bound.
     *
     * Deliberately unconditional: no immobilizer/lv-cutoff/source check,
     * because there is no state in which keeping the starter energised while
     * the engine runs is the safer option. */
    if (running && !was_running) {
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            if (eng->config.channels[ch].is_starter &&
                eng->config.channels[ch].commanded_on) {
                set_commanded_on_raw(eng, ch, false, eng->last_tick_ms);
            }
        }
    }
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

int mc_output_find_brake_channel(const mc_output_config_t *config)
{
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].is_brake) {
            return i;
        }
    }
    return -1;
}

int mc_output_find_indicator_channel(const mc_output_config_t *config, mc_indicator_side_t side)
{
    if (side == MC_INDICATOR_NONE) {
        return -1;
    }
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].indicator == side) {
            return i;
        }
    }
    return -1;
}

int mc_output_find_ignition_channel(const mc_output_config_t *config)
{
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].is_ignition) {
            return i;
        }
    }
    return -1;
}

uint32_t mc_output_config_validate(const mc_output_config_t *config)
{
    uint32_t flags = 0;
    int ignition_count = 0;
    int starter_count = 0;

    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (config->channels[i].is_ignition) {
            ignition_count++;
        }
        if (config->channels[i].is_starter) {
            starter_count++;
        }
    }

    if (ignition_count > 1) {
        flags |= MC_OUT_CFG_MULTIPLE_IGNITION;
    }
    if (starter_count > 1) {
        flags |= MC_OUT_CFG_MULTIPLE_STARTER;
    }

    /* Alternating pairs must be reciprocal. A one-way link is worse than no
     * link: switching A on would put B out, but switching B on would leave A
     * lit, so both beams end up on — the exact state the pair exists to
     * prevent. Cheap to check here, and a config that fails is refused whole
     * rather than half-applied. */
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        int8_t partner = config->channels[i].alternate_channel;
        if (partner < 0) {
            continue;
        }
        if (partner >= MC_OUTPUT_COUNT || partner == (int8_t)i ||
            config->channels[partner].alternate_channel != (int8_t)i) {
            flags |= MC_OUT_CFG_BAD_ALTERNATE;
            continue;
        }
        if (config->channels[i].on_with_ignition &&
            config->channels[partner].on_with_ignition) {
            flags |= MC_OUT_CFG_ALTERNATE_BOTH_IGNITION;
        }
    }

    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        /* Duty is now always meaningful (100 = undimmed), not just in a
         * dedicated PWM mode, so the valid range is checked unconditionally.
         * 0 would mean "on but fully dark", which is never what anyone
         * wants — use commanded_on for off. */
        if (config->channels[i].pwm_duty_pct < 1 || config->channels[i].pwm_duty_pct > 100) {
            flags |= MC_OUT_CFG_BAD_PWM_DUTY;
        }
    }
    if (config->turn_flash_period_ms == 0) {
        flags |= MC_OUT_CFG_BAD_TURN_PERIOD;
    }

    return flags;
}
