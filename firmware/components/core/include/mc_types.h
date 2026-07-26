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

/* Cheat-code / combo sequences: 4-10 presses per the product spec. */
#define MC_COMBO_MAX_LEN 10
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

typedef uint16_t mc_action_id_t;

/* Wrap-safe "has at least `delta_ms` elapsed since `since_ms`, as of `now_ms`". */
static inline bool mc_elapsed_at_least(uint32_t now_ms, uint32_t since_ms, uint32_t delta_ms)
{
    return (uint32_t)(now_ms - since_ms) >= delta_ms;
}
