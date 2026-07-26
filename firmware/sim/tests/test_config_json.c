#include "mc_config_json.h"

#include <assert.h>
#include <stdio.h>
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

    cfg.outputs.channels[0].function = MC_OUT_FUNC_IGNITION;
    strcpy(cfg.outputs.channels[0].name, "Ignition");
    cfg.outputs.channels[3].function = MC_OUT_FUNC_BRAKE;
    strcpy(cfg.outputs.channels[3].name, "Brake Light");
    cfg.outputs.channels[5].function = MC_OUT_FUNC_STARTER;
    cfg.outputs.starter_interlock_input = 2;

    /* Flasher/PWM fields. */
    cfg.outputs.channels[6].function = MC_OUT_FUNC_TURN_L;
    cfg.outputs.channels[6].mode = MC_OUT_MODE_FLASH_TURN;
    cfg.outputs.channels[7].function = MC_OUT_FUNC_AUX;
    cfg.outputs.channels[7].mode = MC_OUT_MODE_PWM;
    cfg.outputs.channels[7].pwm_duty_pct = 60;
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
    cfg.inputs.combos[0].action_id = 42;
    cfg.inputs.short_press_action[2] = 7;
    cfg.inputs.long_press_action[4] = 9;

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

    assert(out.outputs.channels[0].function == MC_OUT_FUNC_HORN);
    /* Remaining channels match defaults. */
    for (int i = 1; i < MC_OUTPUT_COUNT; i++) {
        assert(out.outputs.channels[i].function == def.outputs.channels[i].function);
    }
    assert(out.inputs.timing.debounce_ms == def.inputs.timing.debounce_ms);
}

static void test_unknown_function_maps_to_none(void)
{
    const char *json = "{\"outputs\":{\"channels\":[{\"function\":\"warp_drive\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
    assert(out.outputs.channels[0].function == MC_OUT_FUNC_NONE);
}

/* --- Output modes --- */

static void test_all_mode_strings_roundtrip(void)
{
    const mc_output_mode_t modes[] = {
        MC_OUT_MODE_OFF, MC_OUT_MODE_ON, MC_OUT_MODE_PWM, MC_OUT_MODE_FLASH_TURN, MC_OUT_MODE_FLASH_BRAKE,
    };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        mc_config_t cfg;
        mc_config_default(&cfg);
        cfg.outputs.channels[0].mode = modes[i];

        char *json = mc_config_to_json(&cfg);
        assert(json != NULL);
        mc_config_t back;
        assert(mc_config_from_json(json, strlen(json), &back) == MC_CONFIG_OK);
        assert(back.outputs.channels[0].mode == modes[i]);
        mc_config_json_free(json);
    }
}

static void test_unknown_mode_maps_to_on(void)
{
    /* MC_OUT_MODE_ON is mc_output_config_default()'s own default (see its
     * comment on why ON, not OFF), so an unrecognized/absent mode
     * string falls back to the same value. */
    const char *json = "{\"outputs\":{\"channels\":[{\"mode\":\"warp_drive\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
    assert(out.outputs.channels[0].mode == MC_OUT_MODE_ON);
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
    assert(out.outputs.channels[0].function == MC_OUT_FUNC_HORN);
}

static void test_missing_schema_version_accepted(void)
{
    /* No "schema_version" key at all -- treated as current, same as every
     * other missing field defaulting. */
    const char *json = "{\"outputs\":{\"channels\":[{\"function\":\"horn\"}]}}";
    mc_config_t out;
    assert(mc_config_from_json(json, strlen(json), &out) == MC_CONFIG_OK);
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
    test_future_schema_version_rejected();
    test_current_schema_version_accepted();
    test_missing_schema_version_accepted();
    return 0;
}
