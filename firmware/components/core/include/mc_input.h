#pragma once

/*
 * mc_input — 8-button debounce, short/long/double-press classification,
 * and combo (chord + sequence) detection.
 *
 * Poll-based: call mc_input_poll() at a fixed interval (e.g. every 10ms)
 * with the current raw (active-high logical, i.e. already inverted from
 * the active-low hardware signal) pressed/released state of all 8
 * buttons. All timing is driven by the `now_ms` passed to each poll call,
 * so this is deterministically unit-testable with synthetic input
 * sequences — no real clock or hardware involved.
 *
 * The combo matcher is the generic mechanism behind the unlock cheat-code
 * (AGENTS.md safety requirement #3: a configurable 4-10 press sequence
 * with a timing window) as well as any other chord/sequence binding.
 * This module builds and tests the matcher itself; what a matched combo
 * *does* (unlock, action binding) is wired up by the modules that consume
 * these events (mc_lock for the cheat-code).
 */

#include "mc_types.h"

typedef enum {
    MC_PRESS_SHORT = 0,
    MC_PRESS_LONG,
    MC_PRESS_DOUBLE,
} mc_press_event_type_t;

typedef enum {
    MC_COMBO_CHORD = 0,   /* all buttons held down together within window_ms */
    MC_COMBO_SEQUENCE,    /* ordered short presses, total elapsed <= window_ms */
} mc_combo_type_t;

typedef enum {
    MC_INPUT_EVT_PRESS = 0,
    MC_INPUT_EVT_COMBO,
} mc_input_event_kind_t;

typedef struct {
    mc_input_event_kind_t kind;
    union {
        struct {
            uint8_t button; /* 0..MC_INPUT_COUNT-1 */
            mc_press_event_type_t type;
        } press;
        struct {
            uint8_t combo_index; /* index into the registered combo defs */
            mc_action_id_t action_id;
        } combo;
    } data;
} mc_input_event_t;

typedef struct {
    mc_combo_type_t type;
    uint8_t buttons[MC_COMBO_MAX_LEN];
    uint8_t length;          /* 2..MC_COMBO_MAX_LEN; the cheat-code is 4-10 per spec */
    uint32_t window_ms;
    mc_action_id_t action_id;
} mc_combo_def_t;

typedef struct {
    uint32_t debounce_ms;
    uint32_t long_press_ms;
    uint32_t double_press_gap_ms;
} mc_input_timing_config_t;

typedef struct {
    mc_input_timing_config_t timing;
    mc_combo_def_t combos[MC_COMBO_MAX_DEFS];
    uint8_t combo_count;
    /* Direct button -> action bindings for plain short/long/double
     * presses not part of a combo. MC_ACTION_NONE means unbound. */
    mc_action_id_t short_press_action[MC_INPUT_COUNT];
    mc_action_id_t long_press_action[MC_INPUT_COUNT];
    mc_action_id_t double_press_action[MC_INPUT_COUNT];
} mc_input_config_t;

#define MC_INPUT_EVENT_QUEUE_LEN 16

typedef struct {
    bool raw_state;
    bool stable_state;
    uint32_t raw_change_ms;
    uint32_t press_start_ms;
    bool long_press_fired;
    bool pending_double;
    uint32_t last_release_ms;
} mc_button_state_t;

typedef struct {
    mc_combo_type_t type;
    uint8_t progress;       /* sequence: buttons matched so far. chord: unused. */
    uint32_t start_ms;       /* sequence: time of first matched press. chord: unused. */
    bool chord_fired;        /* chord: already fired for the current hold */
} mc_combo_state_t;

typedef struct {
    mc_input_config_t config;
    mc_button_state_t buttons[MC_INPUT_COUNT];
    mc_combo_state_t combo_state[MC_COMBO_MAX_DEFS];

    mc_input_event_t event_queue[MC_INPUT_EVENT_QUEUE_LEN];
    uint8_t event_head;
    uint8_t event_count;
} mc_input_engine_t;

void mc_input_config_default(mc_input_config_t *out);

void mc_input_init(mc_input_engine_t *eng, const mc_input_config_t *config);

/* Feed the current sampled state of all 8 buttons (true = pressed).
 * Call at a fixed poll interval. May enqueue zero or more events. */
void mc_input_poll(mc_input_engine_t *eng, uint32_t now_ms, const bool raw_pressed[MC_INPUT_COUNT]);

/* Pops the oldest queued event into *out. Returns false if the queue is empty. */
bool mc_input_pop_event(mc_input_engine_t *eng, mc_input_event_t *out);

/* Current debounced (stable) level of one button — true while held. Added
 * for the ignition-switch mode: a maintained switch (not a momentary
 * button) is read as a level rather than press/release events, reusing the
 * same debounce this engine already does. Returns false for an
 * out-of-range button. */
bool mc_input_button_level(const mc_input_engine_t *eng, uint8_t button);
