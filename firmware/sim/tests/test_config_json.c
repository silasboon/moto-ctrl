#include "mc_config_json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_default_roundtrip(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);

    mc_config_t back;
    memset(&back, 0xAA, sizeof(back));
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &back, sizeof(cfg)) == 0);

    mc_config_json_free(json);
}

static void test_modified_roundtrip(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);

    cfg.outputs.channels[0].is_ignition = true; cfg.outputs.channels[0].essential = true;
    strcpy(cfg.outputs.channels[0].name, "Ignition");
    cfg.outputs.channels[3].is_brake = true; cfg.outputs.channels[3].essential = true;
    strcpy(cfg.outputs.channels[3].name, "Brake Light");
    cfg.outputs.channels[5].is_starter = true;
    cfg.outputs.starter_interlock_input = 2;

    /* Flasher/PWM fields. */
    cfg.outputs.channels[6].indicator = MC_INDICATOR_LEFT; cfg.outputs.channels[6].hazard_member = true;
    cfg.outputs.channels[6].behaviour = MC_OUT_BEHAVIOUR_BLINK;
    cfg.outputs.channels[7].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.outputs.channels[7].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.outputs.channels[7].pwm_duty_pct = 60;
    /* schema_version 7. */
    cfg.outputs.channels[7].on_with_ignition = true;
    cfg.outputs.channels[8].alternate_channel = 9;
    cfg.outputs.channels[9].alternate_channel = 8;
    cfg.outputs.brake_switch_input = 3;
    cfg.outputs.turn_auto_cancel_ms = 15000;
    cfg.outputs.turn_flash_period_ms = 500;
    cfg.outputs.brake_flash_pulse_count = 4;
    cfg.outputs.brake_flash_pulse_on_ms = 120;
    cfg.outputs.brake_flash_pulse_off_ms = 40;

    cfg.inputs.timing.debounce_ms = 15;
    cfg.inputs.timing.long_press_ms = 800;
    cfg.inputs.combo_count = 1;
    cfg.inputs.combos[0].type = MC_COMBO_SEQUENCE;
    cfg.inputs.combos[0].length = 4;
    cfg.inputs.combos[0].buttons[0] = 0;
    cfg.inputs.combos[0].buttons[1] = 1;
    cfg.inputs.combos[0].buttons[2] = 0;
    cfg.inputs.combos[0].buttons[3] = 1;
    cfg.inputs.combos[0].window_ms = 5000;
    cfg.inputs.combos[0].actions = (mc_action_list_t){ .actions = { 42 }, .count = 1 };
    cfg.inputs.short_press_actions[2] = (mc_action_list_t){ .actions = { 7 }, .count = 1 };
    cfg.inputs.long_press_actions[4] = (mc_action_list_t){ .actions = { 9 }, .count = 1 };

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);

    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &back, sizeof(cfg)) == 0);

    mc_config_json_free(json);
}

static void test_malformed_json_rejected(void)
{
    const char *bad = "{ this is not json";
    mc_config_t out;
    assert(mc_config_from_json(bad, strlen(bad), &out) == MC_CONFIG_ERR_JSON);
}

static void test_partial_json_uses_defaults(void)
{
    /* Only sets one channel's function; everything else should be default. */
    const char *json = "{\"outputs\":{\"channels\":[{\"function\":\"horn\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);

    mc_config_t def;
    mc_config_default(&def);

    assert((out.outputs.channels[0].behaviour == MC_OUT_BEHAVIOUR_TOGGLE));
    /* Remaining channels match defaults. */
    for (int i = 1; i < MC_OUTPUT_COUNT; i++) {
        assert(memcmp(&out.outputs.channels[i], &def.outputs.channels[i],
                      sizeof(out.outputs.channels[i])) == 0);
    }
    assert(out.inputs.timing.debounce_ms == def.inputs.timing.debounce_ms);
}

static void test_unknown_function_maps_to_none(void)
{
    const char *json = "{\"outputs\":{\"channels\":[{\"function\":\"warp_drive\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
    const mc_output_channel_config_t *ch = &out.outputs.channels[0];
    assert(!ch->is_ignition && !ch->is_starter && !ch->is_brake);
    assert(!ch->essential && !ch->hazard_member);
    assert(ch->indicator == MC_INDICATOR_NONE);
}

/* --- Output modes --- */

static void test_all_mode_strings_roundtrip(void)
{
    const mc_output_behaviour_t modes[] = {
        MC_OUT_BEHAVIOUR_TOGGLE, MC_OUT_BEHAVIOUR_MOMENTARY,
        MC_OUT_BEHAVIOUR_BLINK, MC_OUT_BEHAVIOUR_FLASHER,
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        mc_config_t cfg;
        mc_config_default(&cfg);
        cfg.outputs.channels[0].behaviour = modes[i];

        char *json = mc_config_to_json(&cfg);
        assert(json != NULL);
        mc_config_t back;
        assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
        assert(back.outputs.channels[0].behaviour == modes[i]);
        mc_config_json_free(json);
    }
}

static void test_unknown_mode_maps_to_on(void)
{
    /* TOGGLE is mc_output_config_default()'s own default, so an
     * unrecognised/absent behaviour string falls back to the same value. */
    const char *json = "{\"outputs\":{\"channels\":[{\"behaviour\":\"warp_drive\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
    assert(out.outputs.channels[0].behaviour == MC_OUT_BEHAVIOUR_TOGGLE);
}

/* --- Schema version enforcement --- */

/* mc_config_from_json() is the sole enforcement point for "don't
 * partially apply a config document from a newer schema than this firmware
 * understands" — see mc_config.h's header comment. */
static void test_future_schema_version_rejected(void)
{
    const char *json = "{\"schema_version\":999,\"outputs\":{\"channels\":[{\"function\":\"horn\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_ERR_FUTURE_VERSION);
}

static void test_current_schema_version_accepted(void)
{
    char json[128];
    snprintf(json, sizeof(json), "{\"schema_version\":%d,\"outputs\":{\"channels\":[{\"function\":\"horn\"}]}}",
             MC_CONFIG_SCHEMA_VERSION);
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
    assert((out.outputs.channels[0].behaviour == MC_OUT_BEHAVIOUR_TOGGLE));
}

static void test_missing_schema_version_accepted(void)
{
    /* No "schema_version" key at all -- treated as current, same as every
     * other missing field defaulting. */
    const char *json = "{\"outputs\":{\"channels\":[{\"function\":\"horn\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
}

/* A real schema_version 3 document: press actions are bare numbers, combos
 * carry `action_id`, and there is no `names` array. It must load with the
 * bindings intact, which is the whole v3 -> v4 "migration" (mc_config.h's
 * tolerant-parse doctrine — no versioned C migration code). */
static void test_v3_config_migrates_to_action_lists(void)
{
    const char *v3 =
        "{ \"schema_version\": 3,"
        "  \"inputs\": {"
        "    \"combos\": [ { \"type\": \"chord\", \"buttons\": [0,1],"
        "                    \"window_ms\": 50, \"action_id\": 3 } ],"
        "    \"short_press_action\": [0, 0, 7, 0, 0, 0, 0, 0],"
        "    \"long_press_action\":  [0, 0, 0, 0, 9, 0, 0, 0],"
        "    \"double_press_action\": [0, 0, 0, 0, 0, 0, 0, 0]"
        "  } }";
    mc_config_t out;
    assert(mc_config_from_json(v3, strlen(v3), &out) == MC_CONFIG_OK);

    /* Scalar 7 became a one-entry list on button 2. */
    assert(out.inputs.short_press_actions[2].count == 1);
    assert(out.inputs.short_press_actions[2].actions[0] == 7);
    assert(out.inputs.long_press_actions[4].count == 1);
    assert(out.inputs.long_press_actions[4].actions[0] == 9);

    /* A v3 `0` means unbound, not "an action whose id is 0". */
    assert(out.inputs.short_press_actions[0].count == 0);
    assert(out.inputs.double_press_actions[3].count == 0);

    /* Legacy combo `action_id` still honoured. */
    assert(out.inputs.combo_count == 1);
    assert(out.inputs.combos[0].type == MC_COMBO_CHORD);
    assert(out.inputs.combos[0].actions.count == 1);
    assert(out.inputs.combos[0].actions.actions[0] == MC_ACTION_HAZARD_TOGGLE);

    /* Absent names default to empty, not garbage. */
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        assert(out.inputs.names[i][0] == '\0');
    }
}

/* Multi-action lists and button names survive a round trip, and a name
 * longer than the field is truncated rather than overflowing. */
static void test_action_lists_and_names_roundtrip(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    cfg.inputs.short_press_actions[1] = (mc_action_list_t){
        .actions = { MC_ACTION_OUTPUT_TOGGLE_BASE + 0, MC_ACTION_OUTPUT_TOGGLE_BASE + 11 },
        .count = 2,
    };
    cfg.inputs.double_press_actions[7] = (mc_action_list_t){
        .actions = { MC_ACTION_TURN_L_TOGGLE, MC_ACTION_TURN_R_TOGGLE,
                     MC_ACTION_HAZARD_TOGGLE, MC_ACTION_OUTPUT_TOGGLE_BASE + 5 },
        .count = MC_ACTION_LIST_MAX,
    };
    snprintf(cfg.inputs.names[0], MC_INPUT_NAME_MAX, "Left Bar Top");
    snprintf(cfg.inputs.names[5], MC_INPUT_NAME_MAX, "Kill Switch");

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);
    assert(strlen(json) < MC_CONFIG_JSON_MAX);

    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &back, sizeof(cfg)) == 0);
    mc_config_json_free(json);
}

/* An over-long name from a peer must be truncated, never overflow the
 * fixed-size field. */
static void test_oversized_button_name_truncated(void)
{
    const char *doc =
        "{ \"inputs\": { \"names\": [ \"012345678901234567890123456789ABCDEF\","
        "                             \"\", \"\", \"\", \"\", \"\", \"\", \"\" ] } }";
    mc_config_t out;
    assert(mc_config_from_json(doc, strlen(doc), &out) == MC_CONFIG_OK);
    assert(strlen(out.inputs.names[0]) == MC_INPUT_NAME_MAX - 1);
    assert(out.inputs.names[0][MC_INPUT_NAME_MAX - 1] == '\0');
}

/* More actions than MC_ACTION_LIST_MAX from a peer must be clamped, not
 * written past the array. */
static void test_oversized_action_list_clamped(void)
{
    const char *doc =
        "{ \"inputs\": { \"short_press_action\": [ [1,2,3,256,257,258,259], "
        "0,0,0,0,0,0,0 ] } }";
    mc_config_t out;
    assert(mc_config_from_json(doc, strlen(doc), &out) == MC_CONFIG_OK);
    assert(out.inputs.short_press_actions[0].count == MC_ACTION_LIST_MAX);
}

/* momentary round-trips, and a pre-v5 document (no "momentary" key) loads as
 * latching rather than as garbage. */
static void test_momentary_roundtrip_and_v4_default(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    cfg.outputs.channels[3].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    cfg.outputs.channels[3].behaviour = MC_OUT_BEHAVIOUR_MOMENTARY;
    cfg.outputs.channels[9].is_starter = true;
    cfg.outputs.channels[9].behaviour = MC_OUT_BEHAVIOUR_MOMENTARY;

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);
    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &back, sizeof(cfg)) == 0);
    assert((back.outputs.channels[3].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY));
    assert((back.outputs.channels[9].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY));
    assert((back.outputs.channels[0].behaviour != MC_OUT_BEHAVIOUR_MOMENTARY));
    mc_config_json_free(json);

    /* A v4 channel object has no "momentary" key at all. */
    const char *v4 =
        "{ \"schema_version\": 4, \"outputs\": { \"channels\": ["
        "  { \"function\": \"horn\", \"name\": \"Horn\", \"mode\": \"on\","
        "    \"pwm_duty_pct\": 100, \"commanded_on\": false } ] } }";
    mc_config_t old;
    assert(mc_config_from_json(v4, strlen(v4), &old) == MC_CONFIG_OK);
    assert((old.outputs.channels[0].behaviour == MC_OUT_BEHAVIOUR_TOGGLE));
    assert((old.outputs.channels[0].behaviour != MC_OUT_BEHAVIOUR_MOMENTARY));
}

/* The worst case in mc_config_json.h's size accounting must actually fit,
 * or a fully-configured board can't save its config. */
static void test_worst_case_config_fits_json_max(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        cfg.outputs.channels[i].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
        memset(cfg.outputs.channels[i].name, 'X', MC_OUTPUT_NAME_MAX - 1);
        cfg.outputs.channels[i].name[MC_OUTPUT_NAME_MAX - 1] = '\0';
    }
    for (int i = 0; i < MC_INPUT_COUNT; i++) {
        memset(cfg.inputs.names[i], 'Y', MC_INPUT_NAME_MAX - 1);
        cfg.inputs.names[i][MC_INPUT_NAME_MAX - 1] = '\0';
        for (int k = 0; k < MC_ACTION_LIST_MAX; k++) {
            cfg.inputs.short_press_actions[i].actions[k] = MC_ACTION_OUTPUT_TOGGLE_BASE + k;
            cfg.inputs.long_press_actions[i].actions[k] = MC_ACTION_OUTPUT_TOGGLE_BASE + k;
            cfg.inputs.double_press_actions[i].actions[k] = MC_ACTION_OUTPUT_TOGGLE_BASE + k;
        }
        cfg.inputs.short_press_actions[i].count = MC_ACTION_LIST_MAX;
        cfg.inputs.long_press_actions[i].count = MC_ACTION_LIST_MAX;
        cfg.inputs.double_press_actions[i].count = MC_ACTION_LIST_MAX;
    }
    cfg.inputs.combo_count = MC_COMBO_MAX_DEFS;
    for (int i = 0; i < MC_COMBO_MAX_DEFS; i++) {
        cfg.inputs.combos[i].type = MC_COMBO_SEQUENCE;
        cfg.inputs.combos[i].length = MC_COMBO_MAX_LEN;
        cfg.inputs.combos[i].window_ms = 5000;
        for (int b = 0; b < MC_COMBO_MAX_LEN; b++) {
            cfg.inputs.combos[i].buttons[b] = (uint8_t)(b % MC_INPUT_COUNT);
        }
        for (int k = 0; k < MC_ACTION_LIST_MAX; k++) {
            cfg.inputs.combos[i].actions.actions[k] = MC_ACTION_OUTPUT_TOGGLE_BASE + k;
        }
        cfg.inputs.combos[i].actions.count = MC_ACTION_LIST_MAX;
    }

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);
    size_t len = strlen(json);
    /* Fail loudly with the number if the accounting drifts. */
    if (len >= MC_CONFIG_JSON_MAX) {
        fprintf(stderr, "worst-case config JSON is %zu bytes, MC_CONFIG_JSON_MAX is %d\n",
                len, MC_CONFIG_JSON_MAX);
        assert(0);
    }
    mc_config_t back;
    assert(mc_config_from_json(json, len, &back) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &back, sizeof(cfg)) == 0);
    mc_config_json_free(json);
}

/* Every role a v5 `function` carried must survive the upgrade. The headlight
 * case is the important one: it was only ever protected from the low-voltage
 * cutoff because of its function tag, so the migration has to turn that into
 * an explicit `essential` or ride-safe failure silently regresses. */
static void test_v5_functions_migrate_to_role_flags(void)
{
    const char *v5 =
        "{ \"schema_version\": 5, \"outputs\": { \"channels\": ["
        "  { \"function\": \"ignition\" },"
        "  { \"function\": \"brake\", \"mode\": \"flash_brake\" },"
        "  { \"function\": \"headlight_lo\" },"
        "  { \"function\": \"turn_l\", \"mode\": \"flash_turn\" },"
        "  { \"function\": \"turn_r\", \"mode\": \"flash_turn\" },"
        "  { \"function\": \"starter\", \"momentary\": true },"
        "  { \"function\": \"horn\" },"
        "  { \"function\": \"aux\", \"mode\": \"pwm\", \"pwm_duty_pct\": 40 } ] } }";
    mc_config_t c;
    assert(mc_config_from_json(v5, strlen(v5), &c) == MC_CONFIG_OK);

    assert(c.outputs.channels[0].is_ignition && c.outputs.channels[0].essential);
    assert(c.outputs.channels[1].is_brake && c.outputs.channels[1].essential);
    assert(c.outputs.channels[1].behaviour == MC_OUT_BEHAVIOUR_FLASHER);
    assert(c.outputs.channels[2].essential);
    assert(mc_output_channel_is_essential(&c.outputs.channels[2]));
    assert(c.outputs.channels[3].indicator == MC_INDICATOR_LEFT);
    assert(c.outputs.channels[3].hazard_member);
    assert(c.outputs.channels[3].behaviour == MC_OUT_BEHAVIOUR_BLINK);
    assert(c.outputs.channels[4].indicator == MC_INDICATOR_RIGHT);
    assert(c.outputs.channels[4].hazard_member);
    assert(c.outputs.channels[5].is_starter);
    assert(c.outputs.channels[5].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY);
    assert(!c.outputs.channels[6].essential && !c.outputs.channels[6].is_brake);
    assert(c.outputs.channels[7].behaviour == MC_OUT_BEHAVIOUR_TOGGLE);
    assert(c.outputs.channels[7].pwm_duty_pct == 40);
}

/* schema_version 7 fields survive a round trip, and a pre-v7 document (one
 * with the v6 role flags but no v7 keys) keeps the defaults that mean exactly
 * what it meant: nothing came on with the ignition, no channel had a partner.
 * That is why v7 needed no migration pass. */
static void test_v7_fields_roundtrip_and_default_on_older_documents(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    cfg.outputs.channels[2].on_with_ignition = true;
    cfg.outputs.channels[3].alternate_channel = 4;
    cfg.outputs.channels[4].alternate_channel = 3;

    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);

    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(back.outputs.channels[2].on_with_ignition);
    assert(back.outputs.channels[3].alternate_channel == 4);
    assert(back.outputs.channels[4].alternate_channel == 3);
    assert(back.outputs.channels[5].alternate_channel == -1);
    assert(mc_output_config_validate(&back.outputs) == MC_OUT_CFG_OK);
    free(json);

    /* A v6 document: role flags present, v7 keys absent. */
    const char *v6 =
        "{ \"schema_version\": 6, \"outputs\": { \"channels\": ["
        "  { \"behaviour\": \"toggle\", \"is_ignition\": true },"
        "  { \"behaviour\": \"toggle\", \"essential\": true }"
        "] } }";
    mc_config_t older;
    assert(mc_config_from_json(v6, strlen(v6), &older) == MC_CONFIG_OK);
    assert(!older.outputs.channels[0].on_with_ignition);
    assert(!older.outputs.channels[1].on_with_ignition);
    assert(older.outputs.channels[0].alternate_channel == -1);
    assert(older.outputs.channels[1].alternate_channel == -1);
}

/* A hand-edited or truncated document naming a nonexistent partner must not
 * make the whole config unloadable — it is clamped to "no partner", which
 * validation then accepts. Asymmetry is still caught by validation. */
static void test_out_of_range_alternate_channel_is_dropped(void)
{
    const char *json =
        "{ \"schema_version\": 7, \"outputs\": { \"channels\": ["
        "  { \"behaviour\": \"toggle\", \"alternate_channel\": 99 },"
        "  { \"behaviour\": \"toggle\", \"alternate_channel\": 1 }"
        "] } }";
    mc_config_t c;
    assert(mc_config_from_json(json, strlen(json), &c) == MC_CONFIG_OK);
    assert(c.outputs.channels[0].alternate_channel == -1); /* out of range */
    assert(c.outputs.channels[1].alternate_channel == -1); /* pointed at itself */
    assert(mc_output_config_validate(&c.outputs) == MC_OUT_CFG_OK);
}


/* schema_version 8: the board nickname. Absent from every earlier document,
 * and empty means the factory default — so, like v7, no migration pass. */
static void test_device_name_roundtrip_and_default(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    /* Unset out of the box, and read back as the factory name. */
    assert(cfg.device_name[0] == '\0');
    assert(strcmp(mc_config_effective_device_name(&cfg), MC_DEVICE_NAME_DEFAULT) == 0);

    strcpy(cfg.device_name, "Bonneville");
    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);

    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(strcmp(back.device_name, "Bonneville") == 0);
    assert(strcmp(mc_config_effective_device_name(&back), "Bonneville") == 0);
    free(json);

    /* A pre-v8 document leaves it unset, which reads as the factory name —
     * exactly what such a board was called. */
    const char *v7 = "{ \"schema_version\": 7, \"outputs\": { \"channels\": [] } }";
    mc_config_t older;
    assert(mc_config_from_json(v7, strlen(v7), &older) == MC_CONFIG_OK);
    assert(older.device_name[0] == '\0');
    assert(strcmp(mc_config_effective_device_name(&older), MC_DEVICE_NAME_DEFAULT) == 0);
}

/* An over-long name is truncated, not rejected: a cosmetic overflow must not
 * cost the rider their whole config, same as channel and button names. */
static void test_oversized_device_name_truncated(void)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{ \"schema_version\": 8, \"device_name\": "
             "\"012345678901234567890123456789ABCDEF\" }");
    mc_config_t c;
    assert(mc_config_from_json(json, strlen(json), &c) == MC_CONFIG_OK);
    assert(strlen(c.device_name) == MC_DEVICE_NAME_MAX - 1);
    assert(strncmp(c.device_name, "012345678901234567890123", MC_DEVICE_NAME_MAX - 1) == 0);
}


/* schema_version 9: opt-in voltage-based engine_running detection
 * (mc_diag.h). Absent from every earlier document, and false is exactly
 * what those documents meant -- importing an old export must never
 * silently turn detection on. */
static void test_engine_run_voltage_detection_roundtrip_and_default(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    assert(cfg.diagnostics.engine_run_voltage_detection_enabled == false);

    cfg.diagnostics.engine_run_voltage_detection_enabled = true;
    char *json = mc_config_to_json(&cfg);
    assert(json != NULL);

    mc_config_t back;
    assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
    assert(back.diagnostics.engine_run_voltage_detection_enabled == true);
    mc_config_json_free(json);

    /* A pre-v9 document leaves it unset, which reads as off. */
    const char *v8 = "{ \"schema_version\": 8, \"outputs\": { \"channels\": [] } }";
    mc_config_t older;
    assert(mc_config_from_json(v8, strlen(v8), &older) == MC_CONFIG_OK);
    assert(older.diagnostics.engine_run_voltage_detection_enabled == false);
}

int main(void)
{
    test_default_roundtrip();
    test_modified_roundtrip();
    test_malformed_json_rejected();
    test_partial_json_uses_defaults();
    test_unknown_function_maps_to_none();
    test_all_mode_strings_roundtrip();
    test_unknown_mode_maps_to_on();
    test_v5_functions_migrate_to_role_flags();
    test_future_schema_version_rejected();
    test_current_schema_version_accepted();
    test_missing_schema_version_accepted();
    test_v3_config_migrates_to_action_lists();
    test_action_lists_and_names_roundtrip();
    test_oversized_button_name_truncated();
    test_oversized_action_list_clamped();
    test_momentary_roundtrip_and_v4_default();
    test_worst_case_config_fits_json_max();
    test_v7_fields_roundtrip_and_default_on_older_documents();
    test_out_of_range_alternate_channel_is_dropped();
    test_device_name_roundtrip_and_default();
    test_oversized_device_name_truncated();
    test_engine_run_voltage_detection_roundtrip_and_default();
    return 0;
}
