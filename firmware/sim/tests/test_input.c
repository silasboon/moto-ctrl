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
        .action_id = 42,
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
            assert(evt.data.combo.action_id == 42);
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
        .action_id = 42,
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
        .action_id = 7,
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
        .action_id = 99,
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
    assert(evt.data.combo.action_id == 99);
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

int main(void)
{
    test_debounce_filters_bounce();
    test_short_press();
    test_long_press_fires_while_held();
    test_double_press();
    test_sequence_combo_matches_in_order();
    test_sequence_combo_wrong_button_resets();
    test_sequence_combo_times_out();
    test_chord_combo();
    return 0;
}
