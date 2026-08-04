#pragma once

/*
 * mc_output — the 12-channel output engine.
 *
 * Channel function assignment + friendly names, on/off switching, PWM
 * dimming, and flasher patterns (turn, hazard, brake flasher) all live
 * directly in this module — one module (mc_output_engine_t/mc_output.c)
 * rather than several sibling ones, so the mode field can extend this
 * struct rather than reshaping it as behavior grows.
 *
 * Four safety invariants are enforced here, in the one layer every future
 * caller (hardware buttons now, BLE control later) has to go through:
 *
 *   - Starter protection (AGENTS.md #6): starter output can only be
 *     commanded from MC_OUT_SRC_LOCAL (hardware button path), never
 *     MC_OUT_SRC_REMOTE (app/BLE); it's rejected outright
 *     while `engine_running` is set, and rejected unless a configured
 *     neutral/clutch interlock reports engaged.
 *   - Ride-safe failure (AGENTS.md #1): this module never spontaneously
 *     changes output state on its own — every transition is either an
 *     explicit mc_output_set() call, mc_output_hazard_press(), a turn
 *     auto-cancel expiry (mc_output_tick(), itself only ever turning a
 *     non-essential channel OFF), or mc_output_restore_from_config() at
 *     boot restoring exactly what was last commanded.
 *   - Immobilizer enforcement (AGENTS.md #2): while `immobilized`
 *     is set, turning ON the ignition or starter function is refused from
 *     ANY source. mc_lock is the only caller of
 *     mc_output_set_immobilized() — this module doesn't know about lock
 *     states, only about the one flag it must obey, so a future caller
 *     can't bypass the immobilizer by going around mc_lock.
 *   - Low-voltage cutoff (AGENTS.md #7): while `lv_cutoff` is set,
 *     every non-essential channel (see mc_output_function_is_essential())
 *     is suppressed at the hardware regardless of commanded_on — but
 *     commanded_on/mode are left untouched, so recovery
 *     (mc_output_set_lv_cutoff(eng, false)) auto-restores every channel to
 *     its last-commanded state with no separate "unsuppress" call needed
 *     anywhere else. Unlike `immobilized` (blocks turning ON), this
 *     suppresses the actual driven state. mc_diag is the only
 *     caller of mc_output_set_lv_cutoff().
 *
 * Turn-signal policy (mutual exclusion + auto-cancel) is embedded
 * directly in mc_output_set() rather than a bespoke API, so it applies
 * automatically to any caller (a handlebar button today, any future BLE
 * caller) — consistent with the rest of this file. Hazard genuinely needs
 * its own entry point (mc_output_hazard_press()): "both sides on together"
 * can't be expressed as two mc_output_set() calls without the mutual-
 * exclusion rule cancelling the first one.
 */

#include "mc_types.h"

/* What a channel DOES when its trigger fires. Replaces the old
 * mc_output_function_t/mc_output_mode_t pair (schema_version 6).
 *
 * The function taxonomy (headlight_hi/lo, horn, aux, ...) is gone: a rider
 * names a channel whatever they like, and the only things firmware still
 * needs to know about a channel are the explicit role flags below — the ones
 * that actually carry safety logic. Everything else was a label.
 *
 * PWM dimming is no longer a behaviour but a modifier: pwm_duty_pct < 100
 * dims a channel whenever it is driven on, so it composes with TOGGLE and
 * MOMENTARY instead of excluding them. */
typedef enum {
    /* Latching on/off. The default. */
    MC_OUT_BEHAVIOUR_TOGGLE = 0,
    /* On only while its trigger is held (a hold binding, a held chord, or a
     * maintained switch like the brake lever). Released -> off. */
    MC_OUT_BEHAVIOUR_MOMENTARY,
    /* Blinks at config.turn_flash_period_ms while commanded on — indicators,
     * hazards, and anything the rider wants to blink alongside them. Full
     * on/off switching only, never partial duty (AGENTS.md's PWM/flasher
     * rule), so every lamp type works with no hyperflash workarounds. */
    MC_OUT_BEHAVIOUR_BLINK,
    /* A short attention-pulse burst (config.brake_flash_pulse_*) on the
     * off->on transition, then solid while commanded on stays true; off is
     * always immediate, never mid-pattern. Opt-in per AGENTS.md #5: brake
     * flash patterns are "not legal in all jurisdictions, default OFF." */
    MC_OUT_BEHAVIOUR_FLASHER,
    MC_OUT_BEHAVIOUR_COUNT,
} mc_output_behaviour_t;

/* Which side an indicator is on, or none. This is the one piece of the old
 * function enum that had to survive: turn mutual exclusion and the
 * auto-cancel timer both need to know left from right. A channel that merely
 * blinks along with the hazards (a DRL, say) is NOT an indicator — it sets
 * hazard_member instead, so it never participates in mutual exclusion or
 * auto-cancel. */
typedef enum {
    MC_INDICATOR_NONE = 0,
    MC_INDICATOR_LEFT,
    MC_INDICATOR_RIGHT,
} mc_indicator_side_t;

/* Who is asking for a state change. Only MC_OUT_SRC_LOCAL may command the
 * starter output — see AGENTS.md safety requirement #6. */
typedef enum {
    MC_OUT_SRC_LOCAL = 0,  /* hardware button path */
    MC_OUT_SRC_REMOTE,     /* app/BLE control */
} mc_output_source_t;

typedef enum {
    MC_OUT_OK = 0,
    MC_OUT_ERR_BAD_CHANNEL,
    MC_OUT_ERR_STARTER_REMOTE_BLOCKED,   /* starter commanded from MC_OUT_SRC_REMOTE */
    MC_OUT_ERR_STARTER_ENGINE_RUNNING,   /* starter commanded while engine running */
    MC_OUT_ERR_STARTER_INTERLOCK,        /* neutral/clutch interlock configured and not engaged */
    MC_OUT_ERR_IMMOBILIZED,              /* ignition/starter refused: mc_lock has the bike immobilized */
} mc_output_result_t;

/* Config validation problems, returned as a bitmask from
 * mc_output_config_validate(). A caller (app pin mapper, or firmware
 * applying an imported config) should refuse to apply a config with any
 * bit set. */
typedef enum {
    MC_OUT_CFG_OK = 0,
    MC_OUT_CFG_MULTIPLE_IGNITION = 1u << 0,
    MC_OUT_CFG_MULTIPLE_STARTER = 1u << 1,
    /* Some channel's mode is MC_OUT_MODE_PWM with pwm_duty_pct outside 1-100. */
    MC_OUT_CFG_BAD_PWM_DUTY = 1u << 2,
    /* turn_flash_period_ms == 0 — it's a divisor in the blink-phase calc. */
    MC_OUT_CFG_BAD_TURN_PERIOD = 1u << 3,
    /* Some channel's alternate_channel is out of range, points at itself,
     * or isn't reciprocated by its partner. */
    MC_OUT_CFG_BAD_ALTERNATE = 1u << 4,
    /* Both members of an alternating pair are flagged on_with_ignition —
     * they would fight over which one the ignition lights. */
    MC_OUT_CFG_ALTERNATE_BOTH_IGNITION = 1u << 5,
} mc_output_config_flags_t;

/* Flasher/PWM defaults — see mc_output_config_default(). */
#define MC_OUTPUT_DEFAULT_PWM_DUTY_PCT 100u
#define MC_OUTPUT_DEFAULT_TURN_AUTO_CANCEL_MS 30000u
#define MC_OUTPUT_DEFAULT_TURN_FLASH_PERIOD_MS 700u
#define MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_COUNT 3u
#define MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_ON_MS 150u
#define MC_OUTPUT_DEFAULT_BRAKE_FLASH_PULSE_OFF_MS 50u

typedef struct {
    /* Free text, rider-chosen ("Low Beam", "Heated Grips"). Purely
     * cosmetic — no firmware logic keys off a name. */
    char name[MC_OUTPUT_NAME_MAX];
    mc_output_behaviour_t behaviour;
    bool commanded_on;         /* desired/persisted state */
    /* 1-100. Below 100 the channel is dimmed whenever it is driven on,
     * composing with any behaviour. Opt-in per AGENTS.md's PWM/flasher rule
     * (driver-based LED lamps can misbehave under PWM), and never applied to
     * BLINK/FLASHER patterns, which are always full on/off. */
    uint8_t pwm_duty_pct;

    /* --- Role flags. These, and only these, carry safety logic. ---
     *
     * They replaced the old function enum precisely because the enum forced
     * a rider to mislabel a channel to get the behaviour they needed, and
     * because "essential" was silently inferred from the headlight tags —
     * so naming a channel anything else quietly made it sheddable. */

    /* AGENTS.md #1: never dropped by the low-voltage cutoff. Set this on
     * anything that must not go dark or dead mid-ride — headlight, ignition,
     * brake light, fuel pump. Explicit rather than inferred: dropping the
     * headlight function tag would otherwise have made headlights
     * sheddable, which #1 forbids. */
    bool essential;
    /* AGENTS.md #2: the immobilizer's target. mc_lock reads this channel to
     * decide whether the bike is "running", and mc_output_set() refuses to
     * energize it while immobilized. At most one (validated). */
    bool is_ignition;
    /* AGENTS.md #6: never commandable from the app, inhibited while the
     * engine is running (and force-dropped the moment it starts), and gated
     * behind the neutral/clutch interlock when one is assigned. At most one
     * (validated). */
    bool is_starter;
    /* AGENTS.md #5: the brake light. Target of the brake-switch
     * pass-through, and the channel a FLASHER behaviour is meant for. */
    bool is_brake;
    /* Turn mutual exclusion + auto-cancel apply only to these. */
    mc_indicator_side_t indicator;
    /* Blinks together with the hazards. Indicators are normally members,
     * but so can anything else be — a DRL or an aux light the rider wants
     * flashing during a hazard stop. Membership alone grants no
     * mutual-exclusion or auto-cancel behaviour; only `indicator` does. */
    bool hazard_member;

    /* --- schema_version 7 --- */

    /* Comes on with the ignition: what a key turned to "on" does on a stock
     * bike. DRLs, running lights, an instrument cluster.
     *
     * This flag is about switching ON only. Switching the ignition OFF puts
     * EVERY channel out regardless of this flag (see apply_ignition_off()) —
     * a key that killed only the channels it had lit, and left whatever the
     * rider had switched on by hand still burning, would not be a key.
     *
     * Edge-triggered, never held (see apply_ignition_on()). The rider can
     * still switch a companion off mid-ride and it stays off until the next
     * ignition cycle — a held assertion would mean a DRL could not be turned
     * off without killing the ignition, which is worse than the problem it
     * solves. Restoring persisted state at boot is not an edge and
     * deliberately does not fire either path, keeping
     * mc_output_restore_from_config() a faithful replay of what was last
     * commanded (AGENTS.md #1). */
    bool on_with_ignition;

    /* Channel this one alternates with — hi/lo beam, two DRL colours — or
     * -1 for none. Symmetric: if A names B, B must name A, and
     * mc_output_config_validate() rejects a config where it isn't, because
     * a one-way link means switching one channel on cancels the other but
     * not the reverse, which is worse than no link at all.
     *
     * Turning either member on forces the other off, wherever the command
     * came from (app, button, brake pass-through) — the same shape as turn
     * mutual exclusion, and for the same reason: an invariant enforced in
     * mc_output_set() cannot be bypassed by adding a new caller.
     *
     * A pair is at-most-one-on, NOT exactly-one-on: a direct command may
     * still turn both off, so a rider can black out an aux pair from the
     * app. It is mc_output_alternate_press() that never lands on off. */
    int8_t alternate_channel;
} mc_output_channel_config_t;

typedef struct {
    mc_output_channel_config_t channels[MC_OUTPUT_COUNT];
    /* Input index (0..MC_INPUT_COUNT-1) assigned as the starter's
     * neutral/clutch interlock, or -1 if none is assigned (optional per
     * AGENTS.md #6). Interpreting the input itself is
     * mc_input's job; this engine only consumes the resulting engaged/
     * disengaged flag via mc_output_set_interlock_engaged(). */
    int8_t starter_interlock_input;
    /* Input index (0..MC_INPUT_COUNT-1) assigned as the brake lever/pedal
     * switch, or -1 if none. A maintained switch, read as a
     * level via mc_input_button_level() — mirrors starter_interlock_input.
     * The caller (firmware/main's app_task, firmware/sim's ticker) polls
     * this level each tick and calls mc_output_set() on the BRAKE-function
     * channel found via mc_output_find_channel_by_function() whenever it
     * changes; there is no dedicated "set brake input" API here, since a
     * brake-switch transition is just an ordinary commanded_on transition
     * like any other — the attention-burst pattern is purely a property of
     * that channel's mode (MC_OUT_MODE_FLASH_BRAKE). */
    int8_t brake_switch_input;
    /* Turn-signal auto-cancel timer: 0 = never auto-cancels
     * (manual toggle only). See mc_output_set()'s TURN_L/TURN_R handling. */
    uint32_t turn_auto_cancel_ms;
    /* Full on+off blink cycle period for MC_OUT_MODE_FLASH_TURN, ms. Must
     * be nonzero — mc_output_config_validate() enforces this. */
    uint16_t turn_flash_period_ms;
    /* Brake-flasher attention-pulse burst (MC_OUT_MODE_FLASH_BRAKE),
     * played once on the off->on transition before settling solid.
     * pulse_count == 0 means no visible pulses (goes solid immediately). */
    uint8_t brake_flash_pulse_count;
    uint16_t brake_flash_pulse_on_ms;
    uint16_t brake_flash_pulse_off_ms;
} mc_output_config_t;

/* Hardware access, injected so this module stays portable: firmware/main
 * provides a real GPIO-driving implementation, firmware/sim provides a
 * fake one for tests. */
typedef struct {
    void (*set)(uint8_t channel, bool on, void *ctx);
    /* Optional (NULL if the HAL has no PWM support): drive
     * channel at a steady duty_pct (1-100). Called instead of set() only
     * when the channel's mode is MC_OUT_MODE_PWM and it's actually driven
     * on — flasher patterns never use this (AGENTS.md's PWM/flasher rule:
     * flashers are always full on/off). If NULL, a PWM-mode channel just
     * falls back to plain on/off via set(). */
    void (*set_duty)(uint8_t channel, uint8_t duty_pct, void *ctx);
    void *ctx;
} mc_output_hal_t;

typedef struct {
    mc_output_config_t config;
    mc_output_hal_t hal;
    bool engine_running;         /* set by mc_diag's voltage-based detection */
    bool interlock_engaged;      /* set by mc_input once wired */
    bool immobilized;            /* set by mc_lock while LOCKED */
    bool lv_cutoff;               /* set by mc_diag below the low-voltage threshold */

    /* Flasher/PWM runtime state. */
    uint32_t last_tick_ms;       /* updated by mc_output_tick(); used by mc_output_set()/
                                   * mc_output_hazard_press() to timestamp state that's
                                   * armed outside a tick call — at most one tick (~10ms)
                                   * stale, fine for second-scale timers and blink phase. */
    uint32_t turn_auto_cancel_deadline_ms[MC_OUTPUT_COUNT]; /* 0 = none pending */
    uint32_t brake_burst_started_ms[MC_OUTPUT_COUNT];       /* stamped on a FLASHER channel's off->on edge */
    /* True while mc_output_hazard_press() has the group switched on. Makes
     * every hazard_member blink for the duration regardless of its own
     * behaviour, so a solid-by-nature channel (DRL, aux light) actually
     * flashes with the indicators instead of sitting steady beside them.
     * Cleared by the next hazard press, or by any explicit mc_output_set() on
     * a member. */
    bool hazard_active;
    /* Each hazard_member's commanded_on at the instant the hazards were
     * switched on — bit c for channel c. Ending the hazards puts every member
     * back to this rather than to off.
     *
     * Hazards borrow the group; they don't own it. A DRL that was lit before
     * the hazards started is still wanted after they stop — the rider asked
     * for the hazards to end, not for their running light to go out.
     * Restoring is also what makes signalling a turn out of a hazard stop
     * behave: the far indicator returns to off instead of staying latched on
     * and blinking, which looked like the hazards had never stopped. */
    uint16_t hazard_saved_mask;
} mc_output_engine_t;

void mc_output_init(mc_output_engine_t *eng, const mc_output_config_t *config, mc_output_hal_t hal);

/* Applies every channel's persisted commanded_on state to the hal. Call
 * this as early as possible in boot — see AGENTS.md safety requirement #1
 * (<250ms restore after reboot). */
void mc_output_restore_from_config(mc_output_engine_t *eng);

/* For a TURN_L/TURN_R-function channel, turning on also forces the
 * opposite side off (mutual exclusion) and arms the configured auto-cancel
 * timer (turn_auto_cancel_ms == 0 disables it); turning off clears that
 * channel's pending timer. Every other function is unaffected — plain
 * on/off. */
mc_output_result_t mc_output_set(mc_output_engine_t *eng, uint8_t channel, bool on, mc_output_source_t source);

bool mc_output_get_state(const mc_output_engine_t *eng, uint8_t channel);

/* True iff the channel is commanded on, not currently suppressed by the
 * low-voltage cutoff, and not in the momentarily-dark half of a
 * blink/pulse pattern right now — i.e. exactly what's actually being
 * driven to the HAL at `now_ms`, as opposed to mc_output_get_state()'s
 * persisted "intent". Use this (not mc_output_get_state()) anywhere that
 * needs to know whether a channel is really energized this instant, e.g.
 * mc_diag deciding which channels are meaningful to current-sense this
 * tick — critical for a MC_OUT_MODE_FLASH_TURN channel, whose off-phase
 * must read as "not energized" rather than a spurious open-load fault. */
bool mc_output_get_actual_state(const mc_output_engine_t *eng, uint8_t channel, uint32_t now_ms);

/* Toggles hazards: if either TURN_L or TURN_R is currently off, turns both
 * on together (bypassing the mutual-exclusion rule mc_output_set() applies
 * to a single side); if both are already on, turns both off. No auto-cancel
 * timer is armed either way — hazards stay on until pressed again, same as
 * a real hazard switch (and, like any non-essential channel, still subject
 * to the low-voltage cutoff — an acceptable fallback rather than draining
 * the battery flat). No-op if neither a TURN_L nor a TURN_R channel is
 * configured. */
void mc_output_hazard_press(mc_output_engine_t *eng, uint32_t now_ms);

/* Whether hazards are currently running. Reported on the status wire (byte
 * 15 bit 2) because it cannot be inferred from output_state_mask — hazard
 * members blink, so that mask alternates and a client sampling it can't tell
 * running hazards from stopped ones. */
static inline bool mc_output_hazard_active(const mc_output_engine_t *eng)
{
    return eng->hazard_active;
}

/* Steps an alternating pair (see alternate_channel): whichever member is
 * lit, light the other. If neither is on, lights `channel`.
 *
 * Never lands on "both off" — a headlight pair must not be switchable dark
 * by a mistimed tap at night. Turning a pair off entirely is still possible,
 * just not through this: it takes a direct mc_output_set(..., false), i.e.
 * the app or a binding aimed at the channel itself.
 *
 * Returns MC_OUT_ERR_BAD_CHANNEL if `channel` has no partner configured. All
 * the usual guards still apply, since this goes through mc_output_set(). */
mc_output_result_t mc_output_alternate_press(mc_output_engine_t *eng, uint8_t channel,
                                             mc_output_source_t source);

/* Call every ~10ms, alongside mc_input_poll()/mc_diag_tick()/mc_lock_tick()
 * (before mc_diag_tick(), so its mc_output_get_actual_state() calls this
 * tick already see the current blink phase). Updates the engine's notion
 * of "now", expires any turn channel whose auto-cancel deadline has
 * passed, and re-applies every channel's driven state — this is what
 * actually advances MC_OUT_MODE_FLASH_TURN/FLASH_BRAKE's timing between
 * commanded-on transitions. */
void mc_output_tick(mc_output_engine_t *eng, uint32_t now_ms);

/* Channel index (0..MC_OUTPUT_COUNT-1) with the given role, or -1 if none.
 * Where a role is expected to be unique but isn't, the first match wins —
 * mc_output_config_validate() flags duplicate ignition/starter; a duplicate
 * brake or indicator is not an error, it just means only the first
 * participates in brake-switch pass-through / turn mutual exclusion, and any
 * others stay independently commandable. */
int mc_output_find_brake_channel(const mc_output_config_t *config);
int mc_output_find_indicator_channel(const mc_output_config_t *config, mc_indicator_side_t side);

/* Whether the low-voltage cutoff is currently suppressing non-essential
 * outputs. Distinct from output_state_mask (docs/PROTOCOL.md's status
 * wire): commanded_on/output_state_mask reflect the rider's/app's intent —
 * a real vehicle's switch position doesn't change just because a fuse or
 * protection circuit dropped the load — so the app needs this separate
 * signal to show "battery protection is suppressing some outputs" without
 * misreporting switch position. */
static inline bool mc_output_lv_cutoff_active(const mc_output_engine_t *eng)
{
    return eng->lv_cutoff;
}

void mc_output_set_engine_running(mc_output_engine_t *eng, bool running);
void mc_output_set_interlock_engaged(mc_output_engine_t *eng, bool engaged);

/* mc_lock calls this on entering/leaving LOCKED. While true,
 * mc_output_set() refuses to turn the ignition or starter function ON from
 * any source (AGENTS.md #2). Never blocks turning OFF. */
void mc_output_set_immobilized(mc_output_engine_t *eng, bool immobilized);

/* mc_diag calls this when battery voltage crosses the configured
 * low-voltage cutoff threshold (only while !engine_running — see
 * mc_diag.h). See mc_output_engine_t.lv_cutoff's doc comment for exactly
 * what this suppresses. */
void mc_output_set_lv_cutoff(mc_output_engine_t *eng, bool cutoff_active);

/* AGENTS.md #1's "never drop these mid-ride" set, used by the low-voltage
 * cutoff (AGENTS.md #7) to decide what it may suppress.
 *
 * Now read straight off the channel's `essential` flag rather than inferred
 * from a function tag. `is_ignition` and `is_brake` are folded in
 * unconditionally: those two are non-negotiable under #1, so a config that
 * forgot to tick `essential` on them still cannot shed them.
 *
 * The starter is deliberately NOT essential — it has its own guards
 * (engine-running inhibit, interlock, local-only) and offering starter
 * current during a low-battery event is exactly the wrong tradeoff. */
static inline bool mc_output_channel_is_essential(const mc_output_channel_config_t *ch)
{
    return ch->essential || ch->is_ignition || ch->is_brake;
}

/* Channel index whose `is_ignition` is set, or -1. Config validation
 * guarantees at most one. Used by mc_lock to read whether ignition is live. */
int mc_output_find_ignition_channel(const mc_output_config_t *config);

/* Returns a bitmask of mc_output_config_flags_t problems (0 = OK). Pure
 * function of the config; does not touch hardware. */
uint32_t mc_output_config_validate(const mc_output_config_t *config);

/* Fills `out` with sensible defaults: all channels MC_OUT_FUNC_NONE, off,
 * empty names. */
void mc_output_config_default(mc_output_config_t *out);
