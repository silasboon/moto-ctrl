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
            /* True when this press was consumed by a chord that already
             * fired (see mc_button_state_t.chord_consumed). Consumers must
             * skip the button's own ACTION binding, but must still process
             * the press for everything else.
             *
             * Deliberately a flag on a still-delivered event rather than
             * dropping the event: the unlock cheat-code is fed by short
             * presses, so swallowing them could make the immobilizer
             * un-unlockable for a rider whose cheat-code buttons overlap a
             * chord — AGENTS.md #3 forbids ever locking the rider out. The
             * cheat-code and sequence matchers therefore ignore this flag;
             * only action dispatch honours it. */
            bool action_suppressed;
        } press;
        struct {
            uint8_t combo_index; /* index into the registered combo defs */
            mc_action_list_t actions;
        } combo;
    } data;
} mc_input_event_t;

typedef struct {
    mc_combo_type_t type;
    uint8_t buttons[MC_COMBO_MAX_LEN];
    uint8_t length;          /* 2..MC_COMBO_MAX_LEN; the cheat-code is 4-10 per spec */
    uint32_t window_ms;
    mc_action_list_t actions;
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
    /* Direct button -> action bindings for plain short/long/double presses.
     * A zero `count` means unbound. One press may drive several outputs, so
     * each is a list (mc_types.h). Indexed by button, which is why these
     * stay fixed-size arrays rather than a flat self-describing binding
     * list: it keeps the config JSON small enough for MC_CONFIG_JSON_MAX. */
    mc_action_list_t short_press_actions[MC_INPUT_COUNT];
    mc_action_list_t long_press_actions[MC_INPUT_COUNT];
    mc_action_list_t double_press_actions[MC_INPUT_COUNT];
    /* Rider-assigned button labels, NUL-terminated. Empty means unnamed;
     * the app falls back to "Button N". Purely cosmetic — nothing in the
     * core dispatches on a name. */
    char names[MC_INPUT_COUNT][MC_INPUT_NAME_MAX];
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
    /* Set when a chord containing this button fires, cleared on the next
     * press-down. While set, presses of this button are still emitted but
     * carry action_suppressed, so "press L+R for hazards" doesn't also
     * toggle left and right turn individually.
     *
     * This costs no added latency: a short press is only emitted after
     * double_press_gap_ms elapses (see mc_input_poll), while a chord fires
     * on press-down, so the chord is always known before the constituent
     * short press resolves. Long presses (fired at long_press_ms while
     * held) are likewise later than the chord. */
    bool chord_consumed;
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

/* Replaces the live input config on a running engine — call this whenever
 * mc_config_t.inputs changes (a CONFIG_WRITE commit), the same
 * "engine keeps its own copy, so refresh it" bookkeeping mc_session.c already
 * does for mc_output_engine_t.config and mc_diag_t.config.
 *
 * Without it, mc_input_init()'s boot-time snapshot is what the combo matcher
 * and the press timing keep using forever, so newly configured chords never
 * fire until a reboot. Per-button bindings didn't have this problem because
 * the platform dispatch loop reads them straight out of mc_config_t.
 *
 * Combo matcher progress is reset, because combos[] may have changed length
 * or shape and a stale progress index could refer to a different definition.
 * Debounced BUTTON state is deliberately preserved: zeroing it would make a
 * currently-held switch read as released, which for a held brake lever would
 * drop the brake light — a config import must never change output state
 * (AGENTS.md #1). */
void mc_input_set_config(mc_input_engine_t *eng, const mc_input_config_t *config);

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

/* True while `button` is held past the long-press threshold — i.e. the HOLD
 * gesture is currently active, not merely that it fired once. This is what
 * drives a MOMENTARY output: on at the moment the hold registers, off the
 * instant the button is released.
 *
 * Distinct from mc_input_button_level(), which goes true the moment the
 * button is debounced down. A momentary binding deliberately waits for the
 * hold threshold so a single tap (which also passes through "down") doesn't
 * blip the output — single/double tap bind to toggle/blink instead. */
bool mc_input_hold_active(const mc_input_engine_t *eng, uint8_t button);

/* True while every button of chord `combo_index` is currently held. Drives a
 * MOMENTARY output bound to a held chord (the "both bar buttons for the
 * starter" case). Returns false for a non-chord or out-of-range index.
 *
 * Deliberately a level rather than the chord's fire event: a chord fires once
 * on press-down, which is right for a toggle but useless for something that
 * must drop the moment the rider lets go. */
bool mc_input_chord_held(const mc_input_engine_t *eng, uint8_t combo_index);
