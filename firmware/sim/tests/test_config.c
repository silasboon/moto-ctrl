#include "mc_config.h"
#include "mc_config_json.h"

#include <assert.h>
#include <string.h>

typedef struct {
    uint8_t buf[MC_CONFIG_JSON_MAX];
    size_t len;
    bool has_data;
} mem_store_t;

static mc_config_result_t mem_load(uint8_t *buf, size_t buf_len, size_t *out_len, void *ctx)
{
    mem_store_t *s = (mem_store_t *)ctx;
    if (!s->has_data) {
        return MC_CONFIG_ERR_NOT_FOUND;
    }
    if (buf_len < s->len) {
        return MC_CONFIG_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(buf, s->buf, s->len);
    *out_len = s->len;
    return MC_CONFIG_OK;
}

static mc_config_result_t mem_save(const uint8_t *buf, size_t len, void *ctx)
{
    mem_store_t *s = (mem_store_t *)ctx;
    if (len > sizeof(s->buf)) {
        return MC_CONFIG_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(s->buf, buf, len);
    s->len = len;
    s->has_data = true;
    return MC_CONFIG_OK;
}

static void test_default_has_current_schema_version(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    assert(cfg.schema_version == MC_CONFIG_SCHEMA_VERSION);
}

/* mc_config_load()/mc_config_save() are thin wrappers over
 * mc_config_to_json()/mc_config_from_json() (mc_config_json.c) — there is no
 * mc_config_serialize()/mc_config_deserialize() raw-struct binary path. This
 * test exercises the
 * JSON round trip through mc_config_load/save directly rather than calling
 * mc_config_to_json/from_json itself (that's mc_config_json.c's own test
 * file's job) — this file's job is confirming mc_config.c wires the two
 * together correctly. */
static void test_json_round_trip_through_load_save(void)
{
    mc_config_t cfg;
    mc_config_default(&cfg);
    cfg.outputs.channels[3].is_brake = true; cfg.outputs.channels[3].essential = true;
    strcpy(cfg.outputs.channels[3].name, "Brake Light");
    cfg.inputs.short_press_actions[2] = (mc_action_list_t){ .actions = { 77 }, .count = 1 };

    mem_store_t store = {0};
    mc_config_store_hal_t hal = { .load = mem_load, .save = mem_save, .ctx = &store };
    assert(mc_config_save(hal, &cfg) == MC_CONFIG_OK);

    mc_config_t roundtrip;
    memset(&roundtrip, 0xAA, sizeof(roundtrip)); /* poison to prove load fully overwrites */
    assert(mc_config_load(hal, &roundtrip) == MC_CONFIG_OK);

    assert(memcmp(&cfg, &roundtrip, sizeof(cfg)) == 0);
}

/* A store returning malformed bytes (simulating flash bit-rot) must make
 * mc_config_load() fail rather than silently returning garbage — the
 * platform's "fall back to defaults on any load failure" boot behavior
 * (ride-safe failure, main.c) depends on this. */
static void test_load_fails_on_malformed_stored_data(void)
{
    mem_store_t store = {0};
    store.buf[0] = '{'; /* not valid/complete JSON */
    store.len = 1;
    store.has_data = true;
    mc_config_store_hal_t hal = { .load = mem_load, .save = mem_save, .ctx = &store };

    mc_config_t out;
    assert(mc_config_load(hal, &out) == MC_CONFIG_ERR_JSON);
}

static void test_load_returns_defaults_when_store_empty(void)
{
    mem_store_t store = {0};
    mc_config_store_hal_t hal = { .load = mem_load, .save = mem_save, .ctx = &store };

    mc_config_t cfg;
    memset(&cfg, 0xAA, sizeof(cfg));
    assert(mc_config_load(hal, &cfg) == MC_CONFIG_OK);

    mc_config_t defaults;
    mc_config_default(&defaults);
    assert(memcmp(&cfg, &defaults, sizeof(cfg)) == 0);
}

static void test_save_then_load_round_trips_through_store(void)
{
    mem_store_t store = {0};
    mc_config_store_hal_t hal = { .load = mem_load, .save = mem_save, .ctx = &store };

    mc_config_t cfg;
    mc_config_default(&cfg);
    cfg.outputs.channels[0].is_ignition = true; cfg.outputs.channels[0].essential = true;
    cfg.outputs.channels[0].commanded_on = true;

    assert(mc_config_save(hal, &cfg) == MC_CONFIG_OK);

    mc_config_t loaded;
    assert(mc_config_load(hal, &loaded) == MC_CONFIG_OK);
    assert(memcmp(&cfg, &loaded, sizeof(cfg)) == 0);
}

int main(void)
{
    test_default_has_current_schema_version();
    test_json_round_trip_through_load_save();
    test_load_fails_on_malformed_stored_data();
    test_load_returns_defaults_when_store_empty();
    test_save_then_load_round_trips_through_store();
    return 0;
}
