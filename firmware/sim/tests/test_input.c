#include "mc_input.h"

#include <assert.h>
#include <string.h>

static void all_false(bool raw[MC_INPUT_COUNT])
{
    memset(raw, 0, MC_INPUT_COUNT * sizeof(bool));
}

static void test_debounce_filters_bounce(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[0] = true;  mc_input_poll(&eng, 0, raw);
    raw[0] = false; mc_input_poll(&eng, 5, raw);
    raw[0] = true;  mc_input_poll(&eng, 10, raw);
    raw[0] = false; mc_input_poll(&eng, 15, raw);
    /* Same raw value held long enough to *evaluate* debounce, but it
     * never differed from the already-stable (released) state, so no
     * transition — and therefore no event — should occur. */
    mc_input_poll(&eng, 40, raw);

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == false);
}

static void test_short_press(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[0] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);   /* commits press-down */

    raw[0] = false;
    mc_input_poll(&eng, 50, raw);
    mc_input_poll(&eng, 75, raw);   /* commits release, starts double-press wait */

    mc_input_poll(&eng, 75 + 360, raw); /* double-press gap elapses -> resolves to SHORT */

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.kind == MC_INPUT_EVT_PRESS);
    assert(evt.data.press.button == 0);
    assert(evt.data.press.type == MC_PRESS_SHORT);
    assert(mc_input_pop_event(&eng, &evt) == false);
}

static void test_long_press_fires_while_held(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[0] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw); /* commits press-down, press_start_ms = 25 */

    mc_input_poll(&eng, 25 + cfg.timing.long_press_ms, raw); /* still held -> LONG fires */

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.data.press.type == MC_PRESS_LONG);

    /* Releasing after a long press must not also emit a short/double. */
    raw[0] = false;
    mc_input_poll(&eng, 25 + cfg.timing.long_press_ms + 50, raw);
    mc_input_poll(&eng, 25 + cfg.timing.long_press_ms + 75, raw);
    mc_input_poll(&eng, 25 + cfg.timing.long_press_ms + 75 + 400, raw);

    assert(mc_input_pop_event(&eng, &evt) == false);
}

static void test_double_press(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[0] = true;  mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);              /* commit down #1 */
    raw[0] = false; mc_input_poll(&eng, 50, raw);
    mc_input_poll(&eng, 75, raw);              /* commit up #1, pending double */

    raw[0] = true;  mc_input_poll(&eng, 100, raw);
    mc_input_poll(&eng, 125, raw);             /* commit down #2 */
    raw[0] = false; mc_input_poll(&eng, 150, raw);
    mc_input_poll(&eng, 175, raw);             /* commit up #2, within gap -> DOUBLE */

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.data.press.type == MC_PRESS_DOUBLE);
    assert(evt.data.press.button == 0);

    /* No trailing SHORT should show up later either. */
    mc_input_poll(&eng, 175 + 400, raw);
    assert(mc_input_pop_event(&eng, &evt) == false);
}

static void press_short(mc_input_engine_t *eng, uint8_t button, uint32_t *t, bool raw[MC_INPUT_COUNT])
{
    raw[button] = true;
    mc_input_poll(eng, *t, raw); *t += 25;
    mc_input_poll(eng, *t, raw); *t += 25; /* commit down */

    raw[button] = false;
    mc_input_poll(eng, *t, raw); *t += 25;
    mc_input_poll(eng, *t, raw); *t += 25; /* commit up */

    *t += 360;
    mc_input_poll(eng, *t, raw); /* resolves to SHORT */
    *t += 10;
}

static void test_sequence_combo_matches_in_order(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_SEQUENCE,
        .buttons = {0, 1, 0, 1},
        .length = 4,
        .window_ms = 5000,
        .actions = { .actions = { 42 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    uint32_t t = 0;

    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);
    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);

    mc_input_event_t evt;
    int short_count = 0, combo_count = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_PRESS) {
            assert(evt.data.press.type == MC_PRESS_SHORT);
            short_count++;
        } else {
            assert(evt.data.combo.combo_index == 0);
            assert(evt.data.combo.actions.actions[0] == 42);
            combo_count++;
        }
    }
    assert(short_count == 4);
    assert(combo_count == 1);
}

static void test_sequence_combo_wrong_button_resets(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_SEQUENCE,
        .buttons = {0, 1, 0, 1},
        .length = 4,
        .window_ms = 5000,
        .actions = { .actions = { 42 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    uint32_t t = 0;

    press_short(&eng, 0, &t, raw);
    press_short(&eng, 2, &t, raw); /* wrong button: not buttons[1] (1) -> resets */
    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);
    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);

    mc_input_event_t evt;
    int combo_count = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_COMBO) {
            combo_count++;
        }
    }
    assert(combo_count == 1); /* only the second, complete attempt matches */
}

static void test_sequence_combo_times_out(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_SEQUENCE,
        .buttons = {0, 1},
        .length = 2,
        .window_ms = 100, /* very tight: press_short()'s own gap-wait exceeds this */
        .actions = { .actions = { 7 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    uint32_t t = 0;

    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw); /* arrives well after window_ms from button 0 */

    mc_input_event_t evt;
    int combo_count = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_COMBO) {
            combo_count++;
        }
    }
    assert(combo_count == 0);
}

static void test_chord_combo(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {2, 3},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { 99 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[2] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw); /* button 2 stable down at t=25 */

    raw[3] = true;
    mc_input_poll(&eng, 30, raw);
    mc_input_poll(&eng, 55, raw); /* button 3 stable down at t=55; both within window_ms=50 -> fires */

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.kind == MC_INPUT_EVT_COMBO);
    assert(evt.data.combo.actions.actions[0] == 99);
    assert(mc_input_pop_event(&eng, &evt) == false);

    /* Still held: must not re-fire. */
    mc_input_poll(&eng, 100, raw);
    assert(mc_input_pop_event(&eng, &evt) == false);

    /* Release both, re-press together: fires again. */
    raw[2] = false; raw[3] = false;
    mc_input_poll(&eng, 130, raw);
    mc_input_poll(&eng, 155, raw);

    raw[2] = true; raw[3] = true;
    mc_input_poll(&eng, 160, raw);
    mc_input_poll(&eng, 185, raw);

    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.kind == MC_INPUT_EVT_COMBO);
}

/* The headline case from the product spec: "press the L and R turn buttons
 * together to toggle hazards" must fire the chord and NOT also fire each
 * button's own single-press binding. */
static void test_chord_suppresses_member_single_press_actions(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {2, 3},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { 99 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    /* Both down together -> chord fires. */
    raw[2] = true; raw[3] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);

    /* Release both, then let the double-press gap expire so the pending
     * single presses resolve to SHORT. */
    raw[2] = false; raw[3] = false;
    mc_input_poll(&eng, 60, raw);
    mc_input_poll(&eng, 85, raw);
    mc_input_poll(&eng, 500, raw);

    mc_input_event_t evt;
    int combo_count = 0, suppressed = 0, unsuppressed = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_COMBO) {
            combo_count++;
        } else {
            /* The press event is still DELIVERED — that's what keeps the
             * cheat-code working (layered unlock) — but flagged. */
            if (evt.data.press.action_suppressed) suppressed++;
            else unsuppressed++;
        }
    }
    assert(combo_count == 1);
    assert(suppressed == 2);   /* both chord members */
    assert(unsuppressed == 0); /* neither runs its own binding */
}

/* A button pressed ALONE must still bind normally — suppression is per
 * gesture, not a permanent property of being a chord member. */
static void test_chord_member_pressed_alone_still_binds(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {2, 3},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { 99 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    uint32_t t = 0;

    press_short(&eng, 2, &t, raw); /* button 2 alone, no chord */

    mc_input_event_t evt;
    int presses = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        assert(evt.kind == MC_INPUT_EVT_PRESS);
        assert(evt.data.press.action_suppressed == false);
        presses++;
    }
    assert(presses == 1);
}

/* Regression guard for layered unlock: a chord must never be able to starve
 * the unlock cheat-code. Buttons 0 and 1 form both a chord AND the
 * cheat-code sequence; the sequence must still match, because suppression
 * only marks the event, never drops it. */
static void test_chord_does_not_break_cheatcode_sequence(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 2;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {0, 1},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { 99 }, .count = 1 },
    };
    cfg.combos[1] = (mc_combo_def_t){
        .type = MC_COMBO_SEQUENCE,
        .buttons = {0, 1, 0, 1},
        .length = 4,
        .window_ms = 10000,
        .actions = { .actions = { 42 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    uint32_t t = 0;

    /* Entered one at a time, so the chord never fires — the normal way a
     * rider taps a cheat-code. */
    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);
    press_short(&eng, 0, &t, raw);
    press_short(&eng, 1, &t, raw);

    mc_input_event_t evt;
    int seq_matched = 0, shorts = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_COMBO) {
            if (evt.data.combo.actions.actions[0] == 42) seq_matched++;
        } else {
            shorts++;
        }
    }
    assert(shorts == 4);
    assert(seq_matched == 1);
}

/* One trigger driving several outputs — the "a button can trigger more than
 * one output" requirement. */
static void test_combo_carries_multiple_actions(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {4, 5},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { MC_ACTION_OUTPUT_TOGGLE_BASE + 0,
                                  MC_ACTION_OUTPUT_TOGGLE_BASE + 7,
                                  MC_ACTION_HAZARD_TOGGLE },
                     .count = 3 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    raw[4] = true; raw[5] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);

    mc_input_event_t evt;
    assert(mc_input_pop_event(&eng, &evt) == true);
    assert(evt.kind == MC_INPUT_EVT_COMBO);
    assert(evt.data.combo.actions.count == 3);
    assert(evt.data.combo.actions.actions[0] == MC_ACTION_OUTPUT_TOGGLE_BASE + 0);
    assert(evt.data.combo.actions.actions[1] == MC_ACTION_OUTPUT_TOGGLE_BASE + 7);
    assert(evt.data.combo.actions.actions[2] == MC_ACTION_HAZARD_TOGGLE);
}

/* Long press is fired while the button is still held (at long_press_ms),
 * which is later than the chord's press-down detection — so it must be
 * suppressed too, not just short/double. */
static void test_chord_suppresses_member_long_press(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {2, 3},
        .length = 2,
        .window_ms = 50,
        .actions = { .actions = { 99 }, .count = 1 },
    };
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    raw[2] = true; raw[3] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);          /* chord fires */
    mc_input_poll(&eng, 25 + 700, raw);    /* past long_press_ms (600), still held */

    mc_input_event_t evt;
    int longs = 0, suppressed_longs = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_PRESS && evt.data.press.type == MC_PRESS_LONG) {
            longs++;
            if (evt.data.press.action_suppressed) suppressed_longs++;
        }
    }
    assert(longs == 2);
    assert(suppressed_longs == 2);
}

/* Regression: a chord added AFTER mc_input_init() must fire. mc_input keeps
 * its own copy of the config, so without mc_input_set_config() the matcher
 * kept running off the boot-time snapshot and app-configured chords silently
 * never matched — while per-button bindings appeared to work, because the
 * platform dispatch loop reads those straight out of mc_config_t. */
static void test_set_config_activates_a_newly_added_chord(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg); /* boots with NO combos at all */

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    /* Before: pressing both together matches nothing. */
    raw[2] = true;
    raw[3] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw);
    mc_input_event_t evt;
    while (mc_input_pop_event(&eng, &evt)) {
        assert(evt.kind != MC_INPUT_EVT_COMBO);
    }
    raw[2] = false;
    raw[3] = false;
    mc_input_poll(&eng, 60, raw);
    mc_input_poll(&eng, 90, raw);
    while (mc_input_pop_event(&eng, &evt)) {
    }

    /* Now the app writes a chord, exactly as config_commit does. */
    cfg.combo_count = 1;
    cfg.combos[0] = (mc_combo_def_t){
        .type = MC_COMBO_CHORD,
        .buttons = {2, 3},
        .length = 2,
        .window_ms = 120,
        .actions = { .actions = { MC_ACTION_HAZARD_TOGGLE }, .count = 1 },
    };
    mc_input_set_config(&eng, &cfg);

    /* After: the same gesture fires. */
    raw[2] = true;
    raw[3] = true;
    mc_input_poll(&eng, 1000, raw);
    mc_input_poll(&eng, 1025, raw);

    int combos = 0;
    while (mc_input_pop_event(&eng, &evt)) {
        if (evt.kind == MC_INPUT_EVT_COMBO) {
            assert(evt.data.combo.actions.actions[0] == MC_ACTION_HAZARD_TOGGLE);
            combos++;
        }
    }
    assert(combos == 1);
}

/* A config write must not disturb a button that is currently held. Zeroing
 * the debounce state would make a held brake lever read as released and drop
 * the brake light — ride-safe failure forbids a config import changing outputs. */
static void test_set_config_preserves_held_button_state(void)
{
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_engine_t eng;
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);
    raw[5] = true;
    mc_input_poll(&eng, 0, raw);
    mc_input_poll(&eng, 25, raw); /* button 5 debounced down */
    assert(mc_input_button_level(&eng, 5) == true);

    mc_input_set_config(&eng, &cfg);
    assert(mc_input_button_level(&eng, 5) == true);
}

/* mc_power's hold-awake gate. Idling the loop down mid-gesture would
 * swallow it, and short presses are what feed the unlock cheat-code — so a
 * rider entering a code on a parked bike could be locked out. Every
 * in-flight stage must report activity, and it must clear once genuinely
 * idle or the board would never park at all. */
static void test_activity_pending_covers_every_in_flight_stage(void)
{
    mc_input_engine_t eng;
    mc_input_config_t cfg;
    mc_input_config_default(&cfg);
    mc_input_init(&eng, &cfg);

    bool raw[MC_INPUT_COUNT];
    all_false(raw);

    mc_input_poll(&eng, 0, raw);
    assert(!mc_input_activity_pending(&eng));

    /* Button down, not yet debounced. */
    raw[0] = true;
    mc_input_poll(&eng, 10, raw);
    assert(mc_input_activity_pending(&eng));

    /* Debounced and held. */
    mc_input_poll(&eng, 10 + cfg.timing.debounce_ms + 1u, raw);
    assert(mc_input_activity_pending(&eng));

    /* Released, but inside the double-press gap — the gesture is still
     * resolving, so this must not read as idle. */
    all_false(raw);
    uint32_t released_at = 10 + cfg.timing.debounce_ms + 20u;
    mc_input_poll(&eng, released_at, raw);
    mc_input_poll(&eng, released_at + cfg.timing.debounce_ms + 1u, raw);
    assert(mc_input_activity_pending(&eng));

    /* Gap elapsed: the short press is emitted and now sits in the queue,
     * which is itself activity until something drains it. */
    uint32_t settled = released_at + cfg.timing.debounce_ms + cfg.timing.double_press_gap_ms + 10u;
    mc_input_poll(&eng, settled, raw);
    assert(mc_input_activity_pending(&eng));

    mc_input_event_t evt;
    while (mc_input_pop_event(&eng, &evt)) {
        /* drain */
    }
    mc_input_poll(&eng, settled + 10u, raw);
    assert(!mc_input_activity_pending(&eng));
}

int main(void)
{
    test_activity_pending_covers_every_in_flight_stage();
    test_debounce_filters_bounce();
    test_short_press();
    test_long_press_fires_while_held();
    test_double_press();
    test_sequence_combo_matches_in_order();
    test_sequence_combo_wrong_button_resets();
    test_sequence_combo_times_out();
    test_chord_combo();
    test_chord_suppresses_member_single_press_actions();
    test_chord_member_pressed_alone_still_binds();
    test_chord_does_not_break_cheatcode_sequence();
    test_combo_carries_multiple_actions();
    test_chord_suppresses_member_long_press();
    test_set_config_activates_a_newly_added_chord();
    test_set_config_preserves_held_button_state();
    return 0;
}
