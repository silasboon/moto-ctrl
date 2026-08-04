#pragma once

/*
 * mc_config — the versioned, persisted configuration blob: output engine
 * config, input engine config, and diagnostics config.
 *
 * Storage itself (NVS on-target, a file or in-memory blob for
 * firmware/sim) is injected via mc_config_store_hal_t so this stays
 * portable.
 *
 * Persistence format: the on-device NVS blob IS the same JSON
 * text mc_config_json.c already produces for BLE CONFIG-channel export/
 * import — mc_config_load()/mc_config_save() are thin wrappers around
 * mc_config_from_json()/mc_config_to_json(). This replaced an earlier raw
 * memcpy-of-the-struct binary format whose exact-size check
 * meant a schema bump (adding `diagnostics`, then growing
 * mc_output_config_t) could never carry an old blob forward — every
 * upgrade silently degraded to defaults instead (AGENTS.md #1's "fall back
 * to defaults on any load failure" boot behavior masked the gap, since no
 * hardware/OTA existed yet to make it matter). JSON's per-field parsing
 * with defaults for anything missing and tolerance of anything unrecognized
 * makes this naturally forward/backward-compatible for purely additive
 * schema growth, with no per-version C migration code ever needed again.
 *
 * `schema_version` still exists, but purely as a forward-compat tripwire:
 * mc_config_from_json() rejects a document whose schema_version exceeds
 * MC_CONFIG_SCHEMA_VERSION outright (firmware downgrade reading a newer
 * device's config isn't supported), never partially applies it. That's the
 * full extent of what "migration" means now — see mc_config_json.h.
 */

#include "mc_diag.h"
#include "mc_input.h"
#include "mc_output.h"

#define MC_CONFIG_SCHEMA_VERSION 8

typedef struct {
    uint16_t schema_version;
    /* Board nickname (schema_version 8). Empty means the factory default —
     * see MC_DEVICE_NAME_DEFAULT. Read it through
     * mc_config_effective_device_name() rather than directly, so the
     * empty-means-default rule lives in one place. */
    char device_name[MC_DEVICE_NAME_MAX];
    mc_output_config_t outputs;
    mc_input_config_t inputs;
    mc_diag_config_t diagnostics; /* added at schema_version 2 */
} mc_config_t;

/* The name to advertise: the rider's, or MC_DEVICE_NAME_DEFAULT if unset.
 * Never returns NULL or an empty string — a nameless BLE peripheral is
 * undiscoverable in every phone UI. */
static inline const char *mc_config_effective_device_name(const mc_config_t *cfg)
{
    return (cfg != NULL && cfg->device_name[0] != '\0') ? cfg->device_name
                                                        : MC_DEVICE_NAME_DEFAULT;
}

typedef enum {
    MC_CONFIG_OK = 0,
    MC_CONFIG_ERR_BUFFER_TOO_SMALL,
    MC_CONFIG_ERR_CORRUPT,          /* bad magic/size on deserialize */
    MC_CONFIG_ERR_FUTURE_VERSION,   /* schema_version > MC_CONFIG_SCHEMA_VERSION */
    MC_CONFIG_ERR_STORE_READ,
    MC_CONFIG_ERR_STORE_WRITE,
    MC_CONFIG_ERR_NOT_FOUND,        /* store has no config saved yet */
    MC_CONFIG_ERR_JSON,             /* malformed JSON on import (see mc_config_json.h) */
} mc_config_result_t;

/* Fills `out` with defaults for every sub-config, at the current schema version. */
void mc_config_default(mc_config_t *out);

/* Storage backend, injected: firmware/main backs this with ESP-IDF NVS
 * blob get/set; firmware/sim backs it with an in-memory or file-backed
 * store for tests. */
typedef struct {
    mc_config_result_t (*load)(uint8_t *buf, size_t buf_len, size_t *out_len, void *ctx);
    mc_config_result_t (*save)(const uint8_t *buf, size_t len, void *ctx);
    void *ctx;
} mc_config_store_hal_t;

/* Loads and deserializes the persisted config via `hal`. If the store has
 * nothing saved yet (MC_CONFIG_ERR_NOT_FOUND from hal.load), fills `out`
 * with defaults and returns MC_CONFIG_OK. */
mc_config_result_t mc_config_load(mc_config_store_hal_t hal, mc_config_t *out);

/* Serializes `cfg` and writes it via `hal`. Callers on real hardware
 * should route writes through mc_persist (see mc_persist.h) rather than
 * calling this directly on every change, to respect NVS flash wear. */
mc_config_result_t mc_config_save(mc_config_store_hal_t hal, const mc_config_t *cfg);
