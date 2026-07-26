#pragma once

/*
 * mc_config_json — maps the binary mc_config_t to/from JSON, the wire form
 * of the config service (docs/PROTOCOL.md) and the app's config
 * backup/restore format (AGENTS.md, Config section).
 *
 * The JSON is the interoperable, human-readable representation; mc_config_t
 * is the internal binary form. Keeping the two in one place means the
 * schema is defined once.
 */

#include "mc_config.h"

/* Upper bound on the JSON representation of a config, used to size staging
 * buffers. A full config (12 named channels + input bindings/combos)
 * serializes to well under this. */
#define MC_CONFIG_JSON_MAX 4096

/* Serializes `cfg` to a newly heap-allocated NUL-terminated JSON string.
 * Caller frees with mc_config_json_free(). Returns NULL on allocation
 * failure. */
char *mc_config_to_json(const mc_config_t *cfg);

void mc_config_json_free(char *json);

/* Parses `json` (`len` bytes; NUL-termination not required) into `out`.
 * Missing fields take their default values; unknown enum strings map to
 * their "none" variant. Returns MC_CONFIG_ERR_JSON on a parse failure or a
 * structurally invalid document. Does not enforce semantic rules like
 * "exactly one ignition" — callers should still run
 * mc_output_config_validate() on out->outputs before applying. */
mc_config_result_t mc_config_from_json(const char *json, size_t len, mc_config_t *out);
