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
 * buffers (including mc_session_t.cfg_write_buf, one per concurrent BLE
 * session — so raising this costs RAM per session, not just once).
 *
 * Size at schema_version 4, measured (not estimated) by
 * test_worst_case_config_fits_json_max() in sim/tests/test_config_json.c:
 *   bare default config ...................  2146 bytes
 *   worst case (12 named channels + 8 named
 *   buttons + every press array full + 8
 *   combos of 10 buttons x 4 actions) .....  3729 bytes
 * That does still fit 4096, but with only ~370 bytes spare — one added field
 * away from breaking a fully-configured board's ability to save its config.
 * 6144 buys ~2.4KB of headroom instead. The cost is per concurrent session
 * (3 x 2KB extra .bss), which is why this isn't simply set very large.
 *
 * That test asserts the worst case fits and prints the real number on
 * failure, so this accounting cannot silently drift — but re-read it before
 * raising MC_COMBO_MAX_DEFS or MC_ACTION_LIST_MAX. */
#define MC_CONFIG_JSON_MAX 6144

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
