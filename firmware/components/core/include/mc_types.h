#pragma once

/*
 * Shared constants and basic types for the portable MOTO-CTRL core
 * (mc_output, mc_input, mc_config, mc_persist).
 *
 * Everything in firmware/components/core/ is plain C99 with no ESP-IDF or
 * FreeRTOS dependency, so it can be built and unit-tested by
 * firmware/sim/ on the host. Hardware-specific glue (GPIO, NVS, watchdog)
 * lives in firmware/main/ and includes these headers, never the other way
 * around.
 *
 * Time is passed in explicitly everywhere as a monotonic millisecond
 * tick (`uint32_t now_ms`), supplied by the caller, rather than read
 * internally — this is what makes the state machines deterministically
 * unit-testable without mocking a clock. Callers must compare elapsed
 * time with wrap-safe subtraction, e.g. `(int32_t)(now_ms - then_ms)`.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MC_OUTPUT_COUNT 12
#define MC_INPUT_COUNT 8

#define MC_OUTPUT_NAME_MAX 24

/* Rider-assigned button label, so the app can show "Left Bar, Top" instead
 * of "input 3". Same budget as MC_OUTPUT_NAME_MAX for symmetry. */
#define MC_INPUT_NAME_MAX 24

/* Cheat-code / combo sequences: 4-10 presses per the product spec. */
#define MC_COMBO_MAX_LEN 10
/* Combined cap on chord + sequence definitions. KNOWN LIMIT, recorded
 * deliberately: one trigger can drive several outputs (mc_action_list_t
 * below), but a config can hold at most 8 chords/sequences in total. Raising
 * it costs config JSON bytes against MC_CONFIG_JSON_MAX — see the size
 * accounting in mc_config_json.h. Revisit if real users hit it. */
#define MC_COMBO_MAX_DEFS 8

/* No binding/action assigned. */
#define MC_ACTION_NONE 0

/* Handlebar button -> output actions. mc_input itself doesn't
 * interpret these (see mc_input.h) — main.c/sim/src/main.c's dispatch loop
 * does, the same way it already dispatches MC_PRESS_SHORT to
 * mc_lock_cheatcode_press() for the immobilizer's unlock combo. */
#define MC_ACTION_TURN_L_TOGGLE 1
#define MC_ACTION_TURN_R_TOGGLE 2
#define MC_ACTION_HAZARD_TOGGLE 3

/* Direct per-channel output binding: action id MC_ACTION_OUTPUT_TOGGLE_BASE
 * + N toggles output channel N (0-indexed, N < MC_OUTPUT_COUNT). All 12
 * outputs are electrically identical, so a button binds to a channel
 * directly rather than going through the MC_OUT_FUNC_* indirection the three
 * ids above use — that indirection can't express "the second of three AUX
 * channels", because mc_output_find_channel_by_function() returns only the
 * first match.
 *
 * Deliberately a reserved range in the existing uint16_t action id rather
 * than a new config field, so this adds no config schema version and older
 * configs stay valid (see mc_config.h's schema notes). The toggle still goes
 * through mc_output_set() with MC_OUT_SRC_LOCAL, which is what enforces the
 * immobilizer, the starter engine-running/interlock guards (AGENTS.md #6)
 * and turn mutual exclusion — a binding can never bypass those. */
#define MC_ACTION_OUTPUT_TOGGLE_BASE 0x100

typedef uint16_t mc_action_id_t;

/* How many actions one trigger may fire. A single press or chord can switch
 * several outputs at once (e.g. "ignition + fuel pump"), so a binding is a
 * short list rather than one id. Applied in order, left to right. */
#define MC_ACTION_LIST_MAX 4

typedef struct {
    mc_action_id_t actions[MC_ACTION_LIST_MAX];
    uint8_t count; /* 0 == unbound; entries beyond `count` are ignored */
} mc_action_list_t;

/* Wrap-safe "has at least `delta_ms` elapsed since `since_ms`, as of `now_ms`". */
static inline bool mc_elapsed_at_least(uint32_t now_ms, uint32_t since_ms, uint32_t delta_ms)
{
    return (uint32_t)(now_ms - since_ms) >= delta_ms;
}
