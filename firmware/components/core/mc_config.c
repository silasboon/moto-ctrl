#include "mc_config.h"

#include <stdlib.h>
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

/* The MC_CONFIG_JSON_MAX (4KB) staging buffer is heap-allocated, never a
 * local. On the ESP32-S3 target this runs on the main task during
 * app_main(), whose stack is CONFIG_ESP_MAIN_TASK_STACK_SIZE — a 4KB local
 * here overflowed it on every boot and silently corrupted the neighbouring
 * heap block (the esp_timer task's TCB), which surfaced much later as an
 * assert/LoadProhibited inside vTaskGenericNotifyGiveFromISR. Keep every
 * multi-KB config/keystore/lock buffer off the stack. */
mc_config_result_t mc_config_load(mc_config_store_hal_t hal, mc_config_t *out)
{
    uint8_t *buf = malloc(MC_CONFIG_JSON_MAX);
    if (buf == NULL) {
        return MC_CONFIG_ERR_STORE_READ;
    }
    size_t len = 0;

    mc_config_result_t res = hal.load(buf, MC_CONFIG_JSON_MAX, &len, hal.ctx);
    if (res == MC_CONFIG_ERR_NOT_FOUND) {
        free(buf);
        mc_config_default(out);
        return MC_CONFIG_OK;
    }
    if (res != MC_CONFIG_OK) {
        free(buf);
        return res;
    }

    res = mc_config_from_json((const char *)buf, len, out);
    free(buf);
    return res;
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
