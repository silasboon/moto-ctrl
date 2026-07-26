#include "mc_config_json.h"

#include <string.h>

#include "cJSON.h"

/* --- enum <-> string tables --- */

static const char *const OUTPUT_FUNCTION_NAMES[MC_OUT_FUNC_COUNT] = {
    [MC_OUT_FUNC_NONE] = "none",
    [MC_OUT_FUNC_HEADLIGHT_HI] = "headlight_hi",
    [MC_OUT_FUNC_HEADLIGHT_LO] = "headlight_lo",
    [MC_OUT_FUNC_BRAKE] = "brake",
    [MC_OUT_FUNC_TURN_L] = "turn_l",
    [MC_OUT_FUNC_TURN_R] = "turn_r",
    [MC_OUT_FUNC_HORN] = "horn",
    [MC_OUT_FUNC_IGNITION] = "ignition",
    [MC_OUT_FUNC_STARTER] = "starter",
    [MC_OUT_FUNC_AUX] = "aux",
};

static const char *function_to_string(mc_output_function_t f)
{
    if (f < 0 || f >= MC_OUT_FUNC_COUNT || OUTPUT_FUNCTION_NAMES[f] == NULL) {
        return "none";
    }
    return OUTPUT_FUNCTION_NAMES[f];
}

static mc_output_function_t function_from_string(const char *s)
{
    if (s == NULL) {
        return MC_OUT_FUNC_NONE;
    }
    for (int i = 0; i < MC_OUT_FUNC_COUNT; i++) {
        if (OUTPUT_FUNCTION_NAMES[i] != NULL && strcmp(OUTPUT_FUNCTION_NAMES[i], s) == 0) {
            return (mc_output_function_t)i;
        }
    }
    return MC_OUT_FUNC_NONE;
}

static const char *const OUTPUT_MODE_NAMES[] = {
    [MC_OUT_MODE_OFF] = "off",
    [MC_OUT_MODE_ON] = "on",
    [MC_OUT_MODE_PWM] = "pwm",
    [MC_OUT_MODE_FLASH_TURN] = "flash_turn",
    [MC_OUT_MODE_FLASH_BRAKE] = "flash_brake",
};
#define OUTPUT_MODE_NAMES_COUNT (sizeof(OUTPUT_MODE_NAMES) / sizeof(OUTPUT_MODE_NAMES[0]))

static const char *mode_to_string(mc_output_mode_t m)
{
    if ((size_t)m >= OUTPUT_MODE_NAMES_COUNT || OUTPUT_MODE_NAMES[m] == NULL) {
        return "on";
    }
    return OUTPUT_MODE_NAMES[m];
}

static mc_output_mode_t mode_from_string(const char *s)
{
    if (s != NULL) {
        for (size_t i = 0; i < OUTPUT_MODE_NAMES_COUNT; i++) {
            if (OUTPUT_MODE_NAMES[i] != NULL && strcmp(OUTPUT_MODE_NAMES[i], s) == 0) {
                return (mc_output_mode_t)i;
            }
        }
    }
    return MC_OUT_MODE_ON; /* matches mc_output_config_default()'s default */
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
        cJSON_AddStringToObject(obj, "function", function_to_string(ch->function));
        cJSON_AddStringToObject(obj, "name", ch->name);
        cJSON_AddStringToObject(obj, "mode", mode_to_string(ch->mode));
        cJSON_AddNumberToObject(obj, "pwm_duty_pct", ch->pwm_duty_pct);
        cJSON_AddBoolToObject(obj, "commanded_on", ch->commanded_on);
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
        cJSON_AddNumberToObject(obj, "action_id", def->action_id);
        cJSON_AddItemToArray(combos, obj);
    }

    cJSON *sp = cJSON_AddArrayToObject(inputs, "short_press_action");
    cJSON *lp = cJSON_AddArrayToObject(inputs, "long_press_action");
    cJSON *dp = cJSON_AddArrayToObject(inputs, "double_press_action");
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        cJSON_AddItemToArray(sp, cJSON_CreateNumber(cfg->inputs.short_press_action[i]));
        cJSON_AddItemToArray(lp, cJSON_CreateNumber(cfg->inputs.long_press_action[i]));
        cJSON_AddItemToArray(dp, cJSON_CreateNumber(cfg->inputs.double_press_action[i]));
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

            const cJSON *func = cJSON_GetObjectItemCaseSensitive(obj, "function");
            ch->function = function_from_string(cJSON_IsString(func) ? func->valuestring : NULL);

            const cJSON *name = cJSON_GetObjectItemCaseSensitive(obj, "name");
            if (cJSON_IsString(name) && name->valuestring != NULL) {
                strncpy(ch->name, name->valuestring, MC_OUTPUT_NAME_MAX - 1);
                ch->name[MC_OUTPUT_NAME_MAX - 1] = '\0';
            }

            const cJSON *mode = cJSON_GetObjectItemCaseSensitive(obj, "mode");
            ch->mode = mode_from_string(cJSON_IsString(mode) ? mode->valuestring : NULL);

            ch->pwm_duty_pct = (uint8_t)get_uint(obj, "pwm_duty_pct", ch->pwm_duty_pct);

            const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(obj, "commanded_on");
            ch->commanded_on = cJSON_IsTrue(cmd);
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
            def->action_id = (mc_action_id_t)get_uint(obj, "action_id", 0);
            out->inputs.combo_count++;
        }
    }

    const char *action_keys[3] = { "short_press_action", "long_press_action", "double_press_action" };
    mc_action_id_t *action_arrays[3] = {
        out->inputs.short_press_action,
        out->inputs.long_press_action,
        out->inputs.double_press_action,
    };
    for (int k = 0; k < 3; k++) {
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(inputs, action_keys[k]);
        if (cJSON_IsArray(arr)) {
            int n = cJSON_GetArraySize(arr);
            for (int i = 0; i < n && i < MC_INPUT_COUNT; i++) {
                const cJSON *item = cJSON_GetArrayItem(arr, i);
                if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
                    action_arrays[k][i] = (mc_action_id_t)item->valuedouble;
                }
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
