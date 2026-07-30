#include "mc_config_json.h"

#include <string.h>

#include "cJSON.h"

/* --- enum <-> string tables --- */

static const char *const BEHAVIOUR_NAMES[MC_OUT_BEHAVIOUR_COUNT] = {
    [MC_OUT_BEHAVIOUR_TOGGLE] = "toggle",
    [MC_OUT_BEHAVIOUR_MOMENTARY] = "momentary",
    [MC_OUT_BEHAVIOUR_BLINK] = "blink",
    [MC_OUT_BEHAVIOUR_FLASHER] = "flasher",
};

static const char *behaviour_to_string(mc_output_behaviour_t b)
{
    if ((int)b < 0 || b >= MC_OUT_BEHAVIOUR_COUNT || BEHAVIOUR_NAMES[b] == NULL) {
        return "toggle";
    }
    return BEHAVIOUR_NAMES[b];
}

static mc_output_behaviour_t behaviour_from_string(const char *s)
{
    if (s != NULL) {
        for (int i = 0; i < MC_OUT_BEHAVIOUR_COUNT; i++) {
            if (BEHAVIOUR_NAMES[i] != NULL && strcmp(BEHAVIOUR_NAMES[i], s) == 0) {
                return (mc_output_behaviour_t)i;
            }
        }
    }
    return MC_OUT_BEHAVIOUR_TOGGLE;
}

static const char *indicator_to_string(mc_indicator_side_t side)
{
    switch (side) {
    case MC_INDICATOR_LEFT:  return "left";
    case MC_INDICATOR_RIGHT: return "right";
    default:                 return "none";
    }
}

static mc_indicator_side_t indicator_from_string(const char *s)
{
    if (s != NULL) {
        if (strcmp(s, "left") == 0)  return MC_INDICATOR_LEFT;
        if (strcmp(s, "right") == 0) return MC_INDICATOR_RIGHT;
    }
    return MC_INDICATOR_NONE;
}

/* --- schema_version <= 5 compatibility ---
 *
 * Up to v5 a channel carried `function` (a fixed taxonomy) and `mode`. v6
 * replaced both with a free-text name, a `behaviour`, and explicit role
 * flags, because the taxonomy forced riders to mislabel channels to get the
 * behaviour they wanted — and, worse, silently derived "never shed this under
 * low voltage" from the headlight tags, so any other name was sheddable.
 *
 * Rather than a versioned migration pass, the parser simply falls back to the
 * legacy keys when the new ones are absent (mc_config.h's tolerant-parse
 * doctrine). Mapping, chosen so an existing bike keeps behaving identically:
 *
 *   ignition      -> is_ignition + essential
 *   brake         -> is_brake    + essential
 *   headlight_hi  -> essential          (this is the AGENTS.md #1 case)
 *   headlight_lo  -> essential
 *   turn_l        -> indicator=left  + hazard_member
 *   turn_r        -> indicator=right + hazard_member
 *   starter       -> is_starter
 *   horn/aux/none -> no flags (they never carried any logic)
 *
 *   mode on/pwm   -> behaviour toggle   (pwm becomes duty<100, a modifier)
 *   mode off      -> behaviour toggle   (commanded_on already carries "off")
 *   flash_turn    -> behaviour blink
 *   flash_brake   -> behaviour flasher
 *   momentary:true-> behaviour momentary (the v5 bool)
 */
static void apply_legacy_function(const char *fn, mc_output_channel_config_t *ch)
{
    if (fn == NULL) {
        return;
    }
    if (strcmp(fn, "ignition") == 0) {
        ch->is_ignition = true;
        ch->essential = true;
    } else if (strcmp(fn, "brake") == 0) {
        ch->is_brake = true;
        ch->essential = true;
    } else if (strcmp(fn, "headlight_hi") == 0 || strcmp(fn, "headlight_lo") == 0) {
        ch->essential = true;
    } else if (strcmp(fn, "turn_l") == 0) {
        ch->indicator = MC_INDICATOR_LEFT;
        ch->hazard_member = true;
    } else if (strcmp(fn, "turn_r") == 0) {
        ch->indicator = MC_INDICATOR_RIGHT;
        ch->hazard_member = true;
    } else if (strcmp(fn, "starter") == 0) {
        ch->is_starter = true;
    }
    /* horn, aux, none: pure labels, nothing to carry over. */
}

static mc_output_behaviour_t behaviour_from_legacy_mode(const char *mode)
{
    if (mode == NULL) {
        return MC_OUT_BEHAVIOUR_TOGGLE;
    }
    if (strcmp(mode, "flash_turn") == 0)  return MC_OUT_BEHAVIOUR_BLINK;
    if (strcmp(mode, "flash_brake") == 0) return MC_OUT_BEHAVIOUR_FLASHER;
    return MC_OUT_BEHAVIOUR_TOGGLE; /* on, pwm, off */
}

static const char *combo_type_to_string(mc_combo_type_t t)
{
    return (t == MC_COMBO_CHORD) ? "chord" : "sequence";
}

static mc_combo_type_t combo_type_from_string(const char *s)
{
    if (s != NULL && strcmp(s, "chord") == 0) {
        return MC_COMBO_CHORD;
    }
    return MC_COMBO_SEQUENCE;
}

/* An action list serializes as a JSON array of ids; an unbound list is `[]`
 * rather than being omitted, so the arrays stay positional (index == button
 * number) and a reader never has to guess which button an entry belongs to. */
static cJSON *action_list_to_json(const mc_action_list_t *list)
{
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < list->count && i < MC_ACTION_LIST_MAX; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateNumber(list->actions[i]));
    }
    return arr;
}

static void add_action_list(cJSON *parent, const char *key, const mc_action_list_t *list)
{
    cJSON *arr = action_list_to_json(list);
    if (arr != NULL) {
        cJSON_AddItemToObject(parent, key, arr);
    }
}

/* Reads an action list, accepting either the schema_version 4+ form (an array
 * of ids) or the version 3 form (a single number, meaning a one-entry list).
 * Accepting both is what makes the v3 -> v4 migration free: a config written
 * by older firmware parses with no separate migration pass, consistent with
 * mc_config.h's "tolerant parse instead of versioned migration" doctrine.
 * Action id 0 (MC_ACTION_NONE) is treated as "unbound", not as a real entry,
 * so a v3 `0` becomes an empty list rather than a list containing 0. */
static void parse_action_list(const cJSON *item, mc_action_list_t *out)
{
    out->count = 0;
    if (item == NULL) {
        return;
    }
    if (cJSON_IsNumber(item)) {
        if (item->valuedouble > 0) {
            out->actions[0] = (mc_action_id_t)item->valuedouble;
            out->count = 1;
        }
        return;
    }
    if (!cJSON_IsArray(item)) {
        return;
    }
    int n = cJSON_GetArraySize(item);
    for (int i = 0; i < n && out->count < MC_ACTION_LIST_MAX; i++) {
        const cJSON *a = cJSON_GetArrayItem(item, i);
        if (cJSON_IsNumber(a) && a->valuedouble > 0) {
            out->actions[out->count++] = (mc_action_id_t)a->valuedouble;
        }
    }
}

/* --- serialize --- */

char *mc_config_to_json(const mc_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    cJSON_AddNumberToObject(root, "schema_version", cfg->schema_version);

    /* outputs */
    cJSON *outputs = cJSON_AddObjectToObject(root, "outputs");
    cJSON *channels = cJSON_AddArrayToObject(outputs, "channels");
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        const mc_output_channel_config_t *ch = &cfg->outputs.channels[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", ch->name);
        cJSON_AddStringToObject(obj, "behaviour", behaviour_to_string(ch->behaviour));
        cJSON_AddNumberToObject(obj, "pwm_duty_pct", ch->pwm_duty_pct);
        cJSON_AddBoolToObject(obj, "commanded_on", ch->commanded_on);
        cJSON_AddBoolToObject(obj, "essential", ch->essential);
        cJSON_AddBoolToObject(obj, "is_ignition", ch->is_ignition);
        cJSON_AddBoolToObject(obj, "is_starter", ch->is_starter);
        cJSON_AddBoolToObject(obj, "is_brake", ch->is_brake);
        cJSON_AddStringToObject(obj, "indicator", indicator_to_string(ch->indicator));
        cJSON_AddBoolToObject(obj, "hazard_member", ch->hazard_member);
        cJSON_AddItemToArray(channels, obj);
    }
    cJSON_AddNumberToObject(outputs, "starter_interlock_input", cfg->outputs.starter_interlock_input);
    cJSON_AddNumberToObject(outputs, "brake_switch_input", cfg->outputs.brake_switch_input);
    cJSON_AddNumberToObject(outputs, "turn_auto_cancel_ms", cfg->outputs.turn_auto_cancel_ms);
    cJSON_AddNumberToObject(outputs, "turn_flash_period_ms", cfg->outputs.turn_flash_period_ms);
    cJSON_AddNumberToObject(outputs, "brake_flash_pulse_count", cfg->outputs.brake_flash_pulse_count);
    cJSON_AddNumberToObject(outputs, "brake_flash_pulse_on_ms", cfg->outputs.brake_flash_pulse_on_ms);
    cJSON_AddNumberToObject(outputs, "brake_flash_pulse_off_ms", cfg->outputs.brake_flash_pulse_off_ms);

    /* inputs */
    cJSON *inputs = cJSON_AddObjectToObject(root, "inputs");
    cJSON *timing = cJSON_AddObjectToObject(inputs, "timing");
    cJSON_AddNumberToObject(timing, "debounce_ms", cfg->inputs.timing.debounce_ms);
    cJSON_AddNumberToObject(timing, "long_press_ms", cfg->inputs.timing.long_press_ms);
    cJSON_AddNumberToObject(timing, "double_press_gap_ms", cfg->inputs.timing.double_press_gap_ms);

    cJSON *combos = cJSON_AddArrayToObject(inputs, "combos");
    for (int i = 0; i < cfg->inputs.combo_count; i++) {
        const mc_combo_def_t *def = &cfg->inputs.combos[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", combo_type_to_string(def->type));
        cJSON *buttons = cJSON_AddArrayToObject(obj, "buttons");
        for (int b = 0; b < def->length; b++) {
            cJSON_AddItemToArray(buttons, cJSON_CreateNumber(def->buttons[b]));
        }
        cJSON_AddNumberToObject(obj, "window_ms", def->window_ms);
        add_action_list(obj, "actions", &def->actions);
        cJSON_AddItemToArray(combos, obj);
    }

    /* Per-button action lists. Emitted as arrays-of-arrays at schema_version
     * 4 (they were arrays of plain numbers at 3) — see parse_inputs(), which
     * still accepts either form so a v3 config loads unchanged. */
    cJSON *sp = cJSON_AddArrayToObject(inputs, "short_press_action");
    cJSON *lp = cJSON_AddArrayToObject(inputs, "long_press_action");
    cJSON *dp = cJSON_AddArrayToObject(inputs, "double_press_action");
    cJSON *names = cJSON_AddArrayToObject(inputs, "names");
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        cJSON_AddItemToArray(sp, action_list_to_json(&cfg->inputs.short_press_actions[i]));
        cJSON_AddItemToArray(lp, action_list_to_json(&cfg->inputs.long_press_actions[i]));
        cJSON_AddItemToArray(dp, action_list_to_json(&cfg->inputs.double_press_actions[i]));
        cJSON_AddItemToArray(names, cJSON_CreateString(cfg->inputs.names[i]));
    }

    /* diagnostics */
    cJSON *diag = cJSON_AddObjectToObject(root, "diagnostics");
    cJSON *dchannels = cJSON_AddArrayToObject(diag, "channels");
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        const mc_diag_channel_config_t *dc = &cfg->diagnostics.channels[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "open_load_ma", dc->open_load_ma);
        cJSON_AddNumberToObject(obj, "overcurrent_ma", dc->overcurrent_ma);
        cJSON_AddItemToArray(dchannels, obj);
    }
    cJSON_AddNumberToObject(diag, "lv_cutoff_mv", cfg->diagnostics.lv_cutoff_mv);
    cJSON_AddNumberToObject(diag, "lv_cutoff_hysteresis_mv", cfg->diagnostics.lv_cutoff_hysteresis_mv);
    cJSON_AddNumberToObject(diag, "engine_run_mv", cfg->diagnostics.engine_run_mv);
    cJSON_AddNumberToObject(diag, "engine_run_hysteresis_mv", cfg->diagnostics.engine_run_hysteresis_mv);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

void mc_config_json_free(char *json)
{
    cJSON_free(json);
}

/* --- deserialize helpers --- */

static uint32_t get_uint(const cJSON *obj, const char *key, uint32_t fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        return (uint32_t)item->valuedouble;
    }
    return fallback;
}

static mc_config_result_t parse_outputs(const cJSON *outputs, mc_config_t *out)
{
    if (!cJSON_IsObject(outputs)) {
        return MC_CONFIG_OK; /* absent section: keep defaults */
    }

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(outputs, "channels");
    if (cJSON_IsArray(channels)) {
        int n = cJSON_GetArraySize(channels);
        for (int i = 0; i < n && i < MC_OUTPUT_COUNT; i++) {
            const cJSON *obj = cJSON_GetArrayItem(channels, i);
            if (!cJSON_IsObject(obj)) {
                return MC_CONFIG_ERR_JSON;
            }
            mc_output_channel_config_t *ch = &out->outputs.channels[i];

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
            if (cJSON_IsString(name) && name->valuestring != NULL) {
                strncpy(ch->name, name->valuestring, MC_OUTPUT_NAME_MAX - 1);
                ch->name[MC_OUTPUT_NAME_MAX - 1] = '\0';
            }

            ch->pwm_duty_pct = (uint8_t)get_uint(obj, "pwm_duty_pct", ch->pwm_duty_pct);

            const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(obj, "commanded_on");
            ch->commanded_on = cJSON_IsTrue(cmd);

            /* schema_version 6: behaviour + explicit role flags. A document
             * without them is v5-or-older, so derive from `function`/`mode`
             * (and v5's `momentary` bool) instead — see the mapping comment
             * at the top of this file. Reading the new keys first means a v6
             * document never consults the legacy ones. */
            const cJSON *beh = cJSON_GetObjectItemCaseSensitive(obj, "behaviour");
            if (cJSON_IsString(beh)) {
                ch->behaviour = behaviour_from_string(beh->valuestring);
            } else {
                const cJSON *mode = cJSON_GetObjectItemCaseSensitive(obj, "mode");
                ch->behaviour = behaviour_from_legacy_mode(
                    cJSON_IsString(mode) ? mode->valuestring : NULL);
                const cJSON *mom = cJSON_GetObjectItemCaseSensitive(obj, "momentary");
                if (cJSON_IsTrue(mom)) {
                    ch->behaviour = MC_OUT_BEHAVIOUR_MOMENTARY;
                }
            }

            const cJSON *ess = cJSON_GetObjectItemCaseSensitive(obj, "essential");
            const cJSON *ign = cJSON_GetObjectItemCaseSensitive(obj, "is_ignition");
            const cJSON *stt = cJSON_GetObjectItemCaseSensitive(obj, "is_starter");
            const cJSON *brk = cJSON_GetObjectItemCaseSensitive(obj, "is_brake");
            const cJSON *ind = cJSON_GetObjectItemCaseSensitive(obj, "indicator");
            const cJSON *haz = cJSON_GetObjectItemCaseSensitive(obj, "hazard_member");
            bool has_roles = (ess || ign || stt || brk || ind || haz);
            if (has_roles) {
                ch->essential = cJSON_IsTrue(ess);
                ch->is_ignition = cJSON_IsTrue(ign);
                ch->is_starter = cJSON_IsTrue(stt);
                ch->is_brake = cJSON_IsTrue(brk);
                ch->indicator = indicator_from_string(cJSON_IsString(ind) ? ind->valuestring : NULL);
                ch->hazard_member = cJSON_IsTrue(haz);
            } else {
                const cJSON *func = cJSON_GetObjectItemCaseSensitive(obj, "function");
                apply_legacy_function(cJSON_IsString(func) ? func->valuestring : NULL, ch);
            }
        }
    }

    const cJSON *interlock = cJSON_GetObjectItemCaseSensitive(outputs, "starter_interlock_input");
    if (cJSON_IsNumber(interlock)) {
        int v = (int)interlock->valuedouble;
        if (v >= -1 && v < MC_INPUT_COUNT) {
            out->outputs.starter_interlock_input = (int8_t)v;
        }
    }
    const cJSON *brake_input = cJSON_GetObjectItemCaseSensitive(outputs, "brake_switch_input");
    if (cJSON_IsNumber(brake_input)) {
        int v = (int)brake_input->valuedouble;
        if (v >= -1 && v < MC_INPUT_COUNT) {
            out->outputs.brake_switch_input = (int8_t)v;
        }
    }
    out->outputs.turn_auto_cancel_ms = get_uint(outputs, "turn_auto_cancel_ms", out->outputs.turn_auto_cancel_ms);
    out->outputs.turn_flash_period_ms =
        (uint16_t)get_uint(outputs, "turn_flash_period_ms", out->outputs.turn_flash_period_ms);
    out->outputs.brake_flash_pulse_count =
        (uint8_t)get_uint(outputs, "brake_flash_pulse_count", out->outputs.brake_flash_pulse_count);
    out->outputs.brake_flash_pulse_on_ms =
        (uint16_t)get_uint(outputs, "brake_flash_pulse_on_ms", out->outputs.brake_flash_pulse_on_ms);
    out->outputs.brake_flash_pulse_off_ms =
        (uint16_t)get_uint(outputs, "brake_flash_pulse_off_ms", out->outputs.brake_flash_pulse_off_ms);
    return MC_CONFIG_OK;
}

static mc_config_result_t parse_inputs(const cJSON *inputs, mc_config_t *out)
{
    if (!cJSON_IsObject(inputs)) {
        return MC_CONFIG_OK;
    }

    const cJSON *timing = cJSON_GetObjectItemCaseSensitive(inputs, "timing");
    if (cJSON_IsObject(timing)) {
        out->inputs.timing.debounce_ms = get_uint(timing, "debounce_ms", out->inputs.timing.debounce_ms);
        out->inputs.timing.long_press_ms = get_uint(timing, "long_press_ms", out->inputs.timing.long_press_ms);
        out->inputs.timing.double_press_gap_ms = get_uint(timing, "double_press_gap_ms", out->inputs.timing.double_press_gap_ms);
    }

    const cJSON *combos = cJSON_GetObjectItemCaseSensitive(inputs, "combos");
    if (cJSON_IsArray(combos)) {
        out->inputs.combo_count = 0;
        int n = cJSON_GetArraySize(combos);
        for (int i = 0; i < n && out->inputs.combo_count < MC_COMBO_MAX_DEFS; i++) {
            const cJSON *obj = cJSON_GetArrayItem(combos, i);
            if (!cJSON_IsObject(obj)) {
                return MC_CONFIG_ERR_JSON;
            }
            mc_combo_def_t *def = &out->inputs.combos[out->inputs.combo_count];
            memset(def, 0, sizeof(*def));

            const cJSON *type = cJSON_GetObjectItemCaseSensitive(obj, "type");
            def->type = combo_type_from_string(cJSON_IsString(type) ? type->valuestring : NULL);

            const cJSON *buttons = cJSON_GetObjectItemCaseSensitive(obj, "buttons");
            if (cJSON_IsArray(buttons)) {
                int bn = cJSON_GetArraySize(buttons);
                for (int b = 0; b < bn && def->length < MC_COMBO_MAX_LEN; b++) {
                    const cJSON *bi = cJSON_GetArrayItem(buttons, b);
                    if (cJSON_IsNumber(bi) && bi->valuedouble >= 0 && bi->valuedouble < MC_INPUT_COUNT) {
                        def->buttons[def->length++] = (uint8_t)bi->valuedouble;
                    }
                }
            }
            def->window_ms = get_uint(obj, "window_ms", 0);
            /* "actions" at schema_version 4; "action_id" was the v3 spelling
             * and is still honoured so old configs keep their bindings. */
            const cJSON *acts = cJSON_GetObjectItemCaseSensitive(obj, "actions");
            if (acts == NULL) {
                acts = cJSON_GetObjectItemCaseSensitive(obj, "action_id");
            }
            parse_action_list(acts, &def->actions);
            out->inputs.combo_count++;
        }
    }

    const char *action_keys[3] = { "short_press_action", "long_press_action", "double_press_action" };
    mc_action_list_t *action_arrays[3] = {
        out->inputs.short_press_actions,
        out->inputs.long_press_actions,
        out->inputs.double_press_actions,
    };
    for (int k = 0; k < 3; k++) {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(inputs, action_keys[k]);
        if (cJSON_IsArray(arr)) {
            int n = cJSON_GetArraySize(arr);
            for (int i = 0; i < n && i < MC_INPUT_COUNT; i++) {
                /* parse_action_list() takes either an array (v4) or a bare
                 * number (v3), so both forms land here unchanged. */
                parse_action_list(cJSON_GetArrayItem(arr, i), &action_arrays[k][i]);
            }
        }
    }

    const cJSON *names = cJSON_GetObjectItemCaseSensitive(inputs, "names");
    if (cJSON_IsArray(names)) {
        int n = cJSON_GetArraySize(names);
        for (int i = 0; i < n && i < MC_INPUT_COUNT; i++) {
            const cJSON *item = cJSON_GetArrayItem(names, i);
            if (cJSON_IsString(item) && item->valuestring != NULL) {
                strncpy(out->inputs.names[i], item->valuestring, MC_INPUT_NAME_MAX - 1);
                out->inputs.names[i][MC_INPUT_NAME_MAX - 1] = '\0';
            }
        }
    }
    return MC_CONFIG_OK;
}

static mc_config_result_t parse_diagnostics(const cJSON *diag, mc_config_t *out)
{
    if (!cJSON_IsObject(diag)) {
        return MC_CONFIG_OK; /* absent section: keep defaults */
    }

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(diag, "channels");
    if (cJSON_IsArray(channels)) {
        int n = cJSON_GetArraySize(channels);
        for (int i = 0; i < n && i < MC_OUTPUT_COUNT; i++) {
            const cJSON *obj = cJSON_GetArrayItem(channels, i);
            if (!cJSON_IsObject(obj)) {
                return MC_CONFIG_ERR_JSON;
            }
            mc_diag_channel_config_t *dc = &out->diagnostics.channels[i];
            dc->open_load_ma = (uint16_t)get_uint(obj, "open_load_ma", dc->open_load_ma);
            dc->overcurrent_ma = (uint16_t)get_uint(obj, "overcurrent_ma", dc->overcurrent_ma);
        }
    }

    out->diagnostics.lv_cutoff_mv = (uint16_t)get_uint(diag, "lv_cutoff_mv", out->diagnostics.lv_cutoff_mv);
    out->diagnostics.lv_cutoff_hysteresis_mv =
        (uint16_t)get_uint(diag, "lv_cutoff_hysteresis_mv", out->diagnostics.lv_cutoff_hysteresis_mv);
    out->diagnostics.engine_run_mv = (uint16_t)get_uint(diag, "engine_run_mv", out->diagnostics.engine_run_mv);
    out->diagnostics.engine_run_hysteresis_mv =
        (uint16_t)get_uint(diag, "engine_run_hysteresis_mv", out->diagnostics.engine_run_hysteresis_mv);
    return MC_CONFIG_OK;
}

mc_config_result_t mc_config_from_json(const char *json, size_t len, mc_config_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        return MC_CONFIG_ERR_JSON;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return MC_CONFIG_ERR_JSON;
    }

    /* Reject a document whose declared schema_version is newer
     * than this firmware understands, before touching anything else —
     * this is now the on-device NVS persistence format too (mc_config.c),
     * not just the BLE import path, so a genuinely-too-new document must
     * never be partially applied. Missing/absent "schema_version" is
     * treated as "current" (permissive default), matching every other
     * field's missing-means-default treatment below. */
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (cJSON_IsNumber(sv) && sv->valuedouble > (double)MC_CONFIG_SCHEMA_VERSION) {
        cJSON_Delete(root);
        return MC_CONFIG_ERR_FUTURE_VERSION;
    }

    /* Start from defaults; overlay whatever the document specifies. The
     * produced struct is always at the current schema version regardless
     * of any "schema_version" in the JSON (JSON is an interchange format,
     * not a stored binary layout to migrate). */
    mc_config_default(out);

    mc_config_result_t res = parse_outputs(cJSON_GetObjectItemCaseSensitive(root, "outputs"), out);
    if (res == MC_CONFIG_OK) {
        res = parse_inputs(cJSON_GetObjectItemCaseSensitive(root, "inputs"), out);
    }
    if (res == MC_CONFIG_OK) {
        res = parse_diagnostics(cJSON_GetObjectItemCaseSensitive(root, "diagnostics"), out);
    }

    cJSON_Delete(root);
    return res;
}
