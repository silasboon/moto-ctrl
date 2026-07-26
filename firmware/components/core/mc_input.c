#include "mc_input.h"

#include <string.h>

#define DEFAULT_DEBOUNCE_MS 20
#define DEFAULT_LONG_PRESS_MS 600
#define DEFAULT_DOUBLE_PRESS_GAP_MS 350

void mc_input_config_default(mc_input_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->timing.debounce_ms = DEFAULT_DEBOUNCE_MS;
    out->timing.long_press_ms = DEFAULT_LONG_PRESS_MS;
    out->timing.double_press_gap_ms = DEFAULT_DOUBLE_PRESS_GAP_MS;
    out->combo_count = 0;
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        out->short_press_action[i] = MC_ACTION_NONE;
        out->long_press_action[i] = MC_ACTION_NONE;
        out->double_press_action[i] = MC_ACTION_NONE;
    }
}

void mc_input_init(mc_input_engine_t *eng, const mc_input_config_t *config)
{
    memset(eng, 0, sizeof(*eng));
    eng->config = *config;
}

static void enqueue(mc_input_engine_t *eng, mc_input_event_t evt)
{
    if (eng->event_count >= MC_INPUT_EVENT_QUEUE_LEN) {
        /* Queue full: drop the new event rather than overwrite an older,
         * possibly safety-relevant one (e.g. an in-progress cheat code). */
        return;
    }
    uint8_t tail = (uint8_t)((eng->event_head + eng->event_count) % MC_INPUT_EVENT_QUEUE_LEN);
    eng->event_queue[tail] = evt;
    eng->event_count++;
}

bool mc_input_pop_event(mc_input_engine_t *eng, mc_input_event_t *out)
{
    if (eng->event_count == 0) {
        return false;
    }
    *out = eng->event_queue[eng->event_head];
    eng->event_head = (uint8_t)((eng->event_head + 1) % MC_INPUT_EVENT_QUEUE_LEN);
    eng->event_count--;
    return true;
}

bool mc_input_button_level(const mc_input_engine_t *eng, uint8_t button)
{
    if (button >= MC_INPUT_COUNT) {
        return false;
    }
    return eng->buttons[button].stable_state;
}

static void emit_press(mc_input_engine_t *eng, uint8_t button, mc_press_event_type_t type)
{
    mc_input_event_t evt = {0};
    evt.kind = MC_INPUT_EVT_PRESS;
    evt.data.press.button = button;
    evt.data.press.type = type;
    enqueue(eng, evt);
}

static void emit_combo(mc_input_engine_t *eng, uint8_t combo_index)
{
    mc_input_event_t evt = {0};
    evt.kind = MC_INPUT_EVT_COMBO;
    evt.data.combo.combo_index = combo_index;
    evt.data.combo.action_id = eng->config.combos[combo_index].action_id;
    enqueue(eng, evt);
}

static void handle_sequence_press(mc_input_engine_t *eng, uint8_t button, uint32_t now_ms)
{
    for (uint8_t i = 0; i < eng->config.combo_count; i++) {
        const mc_combo_def_t *def = &eng->config.combos[i];
        if (def->type != MC_COMBO_SEQUENCE || def->length == 0) {
            continue;
        }
        mc_combo_state_t *st = &eng->combo_state[i];

        if (st->progress > 0 && mc_elapsed_at_least(now_ms, st->start_ms, def->window_ms)) {
            st->progress = 0; /* timed out */
        }

        uint8_t expect = def->buttons[st->progress];
        if (expect == button) {
            if (st->progress == 0) {
                st->start_ms = now_ms;
            }
            st->progress++;
            if (st->progress >= def->length) {
                emit_combo(eng, i);
                st->progress = 0;
            }
        } else {
            /* Wrong button: reset, but allow immediately restarting the
             * sequence if this press is also its first button. */
            st->progress = (def->buttons[0] == button) ? 1 : 0;
            if (st->progress == 1) {
                st->start_ms = now_ms;
            }
        }
    }
}

static void handle_chord_state(mc_input_engine_t *eng)
{
    for (uint8_t i = 0; i < eng->config.combo_count; i++) {
        const mc_combo_def_t *def = &eng->config.combos[i];
        if (def->type != MC_COMBO_CHORD || def->length == 0) {
            continue;
        }
        mc_combo_state_t *st = &eng->combo_state[i];

        bool all_down = true;
        uint32_t min_start = 0, max_start = 0;
        for (uint8_t k = 0; k < def->length; k++) {
            uint8_t btn = def->buttons[k];
            if (btn >= MC_INPUT_COUNT || !eng->buttons[btn].stable_state) {
                all_down = false;
                break;
            }
            uint32_t start = eng->buttons[btn].press_start_ms;
            if (k == 0) {
                min_start = max_start = start;
            } else {
                if ((int32_t)(start - min_start) < 0) min_start = start;
                if ((int32_t)(start - max_start) > 0) max_start = start;
            }
        }

        if (!all_down) {
            st->chord_fired = false;
            continue;
        }

        if (!st->chord_fired && (max_start - min_start) <= def->window_ms) {
            emit_combo(eng, i);
            st->chord_fired = true;
        }
    }
}

void mc_input_poll(mc_input_engine_t *eng, uint32_t now_ms, const bool raw_pressed[MC_INPUT_COUNT])
{
    for (uint8_t b = 0; b < MC_INPUT_COUNT; b++) {
        mc_button_state_t *st = &eng->buttons[b];
        bool raw = raw_pressed[b];

        if (raw != st->raw_state) {
            st->raw_state = raw;
            st->raw_change_ms = now_ms;
        } else if (st->raw_state != st->stable_state &&
                   mc_elapsed_at_least(now_ms, st->raw_change_ms, eng->config.timing.debounce_ms)) {
            st->stable_state = st->raw_state;

            if (st->stable_state) {
                /* Debounced press-down. */
                st->press_start_ms = now_ms;
                st->long_press_fired = false;
            } else {
                /* Debounced release. */
                if (!st->long_press_fired) {
                    if (st->pending_double &&
                        !mc_elapsed_at_least(now_ms, st->last_release_ms, eng->config.timing.double_press_gap_ms + 1)) {
                        emit_press(eng, b, MC_PRESS_DOUBLE);
                        st->pending_double = false;
                    } else {
                        st->pending_double = true;
                        st->last_release_ms = now_ms;
                    }
                }
            }
        }

        /* Long-press fires while still held, as soon as the threshold is
         * crossed, not on release. */
        if (st->stable_state && !st->long_press_fired &&
            mc_elapsed_at_least(now_ms, st->press_start_ms, eng->config.timing.long_press_ms)) {
            st->long_press_fired = true;
            st->pending_double = false; /* a long press cancels any pending double-press wait */
            emit_press(eng, b, MC_PRESS_LONG);
        }

        /* A pending single-press that never got its double-press partner
         * within the gap resolves to a short press. Sequence combos (the
         * cheat-code mechanism) match on short presses, so feed the
         * matcher right here rather than re-scanning events afterward. */
        if (st->pending_double && mc_elapsed_at_least(now_ms, st->last_release_ms, eng->config.timing.double_press_gap_ms)) {
            st->pending_double = false;
            emit_press(eng, b, MC_PRESS_SHORT);
            handle_sequence_press(eng, b, now_ms);
        }
    }

    handle_chord_state(eng);
}
