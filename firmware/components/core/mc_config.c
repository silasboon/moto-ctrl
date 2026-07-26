#include "mc_config.h"

#include <string.h>

#include "mc_config_json.h"

void mc_config_default(mc_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->schema_version = MC_CONFIG_SCHEMA_VERSION;
    mc_output_config_default(&out->outputs);
    mc_input_config_default(&out->inputs);
    mc_diag_config_default(&out->diagnostics);
}

/* Persistence is JSON (mc_config_json.c), the same tolerant
 * format already used for BLE CONFIG-channel export/import — see
 * mc_config.h's header comment. mc_config_from_json() already parses each
 * field independently with defaults for anything missing and ignores
 * anything unrecognized, so no separate binary migration path is needed:
 * loading an older or newer-but-still-understood document just works.
 * A document whose schema_version exceeds what this firmware understands is
 * still rejected outright (MC_CONFIG_ERR_FUTURE_VERSION, enforced inside
 * mc_config_from_json() itself now, not here). */

mc_config_result_t mc_config_load(mc_config_store_hal_t hal, mc_config_t *out)
{
    uint8_t buf[MC_CONFIG_JSON_MAX];
    size_t len = 0;

    mc_config_result_t res = hal.load(buf, sizeof(buf), &len, hal.ctx);
    if (res == MC_CONFIG_ERR_NOT_FOUND) {
        mc_config_default(out);
        return MC_CONFIG_OK;
    }
    if (res != MC_CONFIG_OK) {
        return res;
    }

    return mc_config_from_json((const char *)buf, len, out);
}

mc_config_result_t mc_config_save(mc_config_store_hal_t hal, const mc_config_t *cfg)
{
    char *json = mc_config_to_json(cfg);
    if (json == NULL) {
        return MC_CONFIG_ERR_BUFFER_TOO_SMALL;
    }
    mc_config_result_t res = hal.save((const uint8_t *)json, strlen(json), hal.ctx);
    mc_config_json_free(json);
    return res;
}
