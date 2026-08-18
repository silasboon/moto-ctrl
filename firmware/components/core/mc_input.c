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
    /* memset above already zeroed the action lists (count 0 == unbound) and
     * the name strings (empty == unnamed); spelled out here so the intent
     * survives someone adding a non-zero-default field later. */
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        out->short_press_actions[i].count = 0;
        out->long_press_actions[i].count = 0;
        out->double_press_actions[i].count = 0;
        out->names[i][0] = '\0';
    }
}

void mc_input_init(mc_input_engine_t *eng, const mc_input_config_t *config)
{
    memset(eng, 0, sizeof(*eng));
    eng->config = *config;
}

void mc_input_set_config(mc_input_engine_t *eng, const mc_input_config_t *config)
{
    eng->config = *config;
    /* Stale matcher progress could index a combo definition that no longer
     * exists or has a different length — see mc_input.h. */
    memset(eng->combo_state, 0, sizeof(eng->combo_state));
    /* Chord membership may have changed, so a leftover suppression flag would
     * silently swallow the next binding on that button. Only this flag is
     * cleared; the debounce/hold fields around it must survive. */
    for (uint8_t b = 0; b < MC_INPUT_COUNT; b++) {
        eng->buttons[b].chord_consumed = false;
    }
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

bool mc_input_activity_pending(const mc_input_engine_t *eng)
{
    if (eng->event_count > 0) {
        return true;
    }
    for (uint8_t i = 0; i < MC_INPUT_COUNT; i++) {
        const mc_button_state_t *st = &eng->buttons[i];
        /* raw_state as well as stable_state: a press that hasn't debounced
         * yet is still a press in progress, and is exactly the moment a
         * GPIO wake hands control back to the poll loop. */
        if (st->raw_state || st->stable_state || st->pending_double) {
            return true;
        }
    }
    for (uint8_t i = 0; i < eng->config.combo_count; i++) {
        const mc_combo_state_t *cs = &eng->combo_state[i];
        if (cs->progress > 0 || cs->chord_fired) {
            return true;
        }
    }
    return false;
}

bool mc_input_hold_active(const mc_input_engine_t *eng, uint8_t button)
{
    if (button >= MC_INPUT_COUNT) {
        return false;
    }
    /* long_press_fired is set when the threshold is crossed and cleared on
     * the next press-down, so ANDing it with the live level gives exactly
     * "the hold gesture is happening right now". */
    return eng->buttons[button].stable_state && eng->buttons[button].long_press_fired;
}

bool mc_input_chord_held(const mc_input_engine_t *eng, uint8_t combo_index)
{
    if (combo_index >= MC_COMBO_MAX_DEFS || combo_index >= eng->config.combo_count) {
        return false;
    }
    const mc_combo_def_t *def = &eng->config.combos[combo_index];
    if (def->type != MC_COMBO_CHORD || def->length == 0) {
        return false;
    }
    for (uint8_t k = 0; k < def->length; k++) {
        uint8_t btn = def->buttons[k];
        if (btn >= MC_INPUT_COUNT || !eng->buttons[btn].stable_state) {
            return false;
        }
    }
    return true;
}

static void emit_press(mc_input_engine_t *eng, uint8_t button, mc_press_event_type_t type)
{
    mc_input_event_t evt = {0};
    evt.kind = MC_INPUT_EVT_PRESS;
    evt.data.press.button = button;
    evt.data.press.type = type;
    /* Emitted either way — only the action binding is suppressed. See
     * mc_input.h: the cheat-code must keep seeing these presses. */
    evt.data.press.action_suppressed = eng->buttons[button].chord_consumed;
    enqueue(eng, evt);
}

static void emit_combo(mc_input_engine_t *eng, uint8_t combo_index)
{
    mc_input_event_t evt = {0};
    evt.kind = MC_INPUT_EVT_COMBO;
    evt.data.combo.combo_index = combo_index;
    evt.data.combo.actions = eng->config.combos[combo_index].actions;
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
            /* Mark members before emitting so any press of theirs that
             * resolves later carries action_suppressed — the chord fires on
             * press-down, which is always earlier than a short press
             * (deferred by double_press_gap_ms) or a long press (fired at
             * long_press_ms). Chord-only: a matched SEQUENCE cannot suppress
             * its members retroactively, because their press events were
             * already delivered on earlier polls. */
            for (uint8_t k = 0; k < def->length; k++) {
                uint8_t btn = def->buttons[k];
                if (btn < MC_INPUT_COUNT) {
                    eng->buttons[btn].chord_consumed = true;
                }
            }
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
                /* Debounced press-down. A fresh press is a fresh gesture:
                 * clear any chord suppression left over from the previous
                 * one, so releasing and re-pressing a chord member binds
                 * normally again. */
                st->press_start_ms = now_ms;
                st->long_press_fired = false;
                st->chord_consumed = false;
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
