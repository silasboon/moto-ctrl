#pragma once

/*
 * mc_diag — current-sense diagnostics (IS mux driver, current
 * calibration, faults, blown-bulb detection), plus the two safety signals
 * that ride the same two analog lines:
 *   - Starter protection's engine_running signal (voltage-based charging
 *     detection).
 *   - Battery protection's low-voltage cutoff (disables non-essential
 *     outputs below a configurable threshold; never the unlock path).
 *
 * Both shared analog lines are described in hardware/PINOUT.md's
 * "Diagnostics readout procedure":
 *   - PROFET_IS (one shared current-sense ADC line, muxed via PROFET_DSEL +
 *     one PROFET device's DEN at a time — only ON channels are meaningful
 *     to sample; an OFF PROFET reads ~0 and is never fault-classified).
 *     I_load[mA] ~= (V_IS[mV] / 2000) * kILIS (2kOhm sense resistor R3;
 *     kILIS is the BTS7008-2EPA datasheet's current-sense ratio).
 *   - VSENSE_BAT (battery divider ADC line).
 *
 * Open-load/overcurrent thresholds are per-channel, configurable, and
 * learnable rather than hardcoded to incandescent-bulb current
 * assumptions — mc_diag_learn() sets a threshold from a real measured
 * reading rather than a guessed constant.
 *
 * Like every other core module: time is injected, no ESP-IDF/FreeRTOS
 * dependency, fully host-testable. The actual mux-select + ADC read is
 * behind an injected HAL (mc_diag_hal_t, mirrors mc_output_hal_t) so this
 * file stays portable — firmware/main/diag_hal.c provides the real
 * DSEL/DEN-sequenced ADC read, firmware/sim provides one that reads back
 * GUI/test-injected values (see firmware/sim/src/main.c's doc comment on
 * why its default calibration is deliberately an identity mapping).
 *
 * Ticked from the same ~10ms task as mc_input_poll/mc_lock_tick.
 * mc_diag_tick() is the ONLY thing in the system that calls
 * mc_output_set_engine_running() and mc_output_set_lv_cutoff() — mc_lock and
 * mc_output only ever read those flags, keeping this the single producer
 * (mirrors how mc_lock is the sole caller of mc_output_set_immobilized()).
 */

#include "mc_output.h"
#include "mc_types.h"

typedef enum {
    MC_DIAG_FAULT_NONE = 0,
    MC_DIAG_FAULT_OPEN_LOAD,   /* channel actually energized but current < open_load_ma */
    MC_DIAG_FAULT_OVERCURRENT, /* current > overcurrent_ma */
} mc_diag_fault_t;

/* Defaults used by mc_diag_config_default(). Deliberately generic
 * placeholders, not a guess at any particular bulb — these are learnable
 * per channel via mc_diag_learn(), never hardcoded to an assumed load. */
#define MC_DIAG_DEFAULT_OPEN_LOAD_MA 50u
#define MC_DIAG_DEFAULT_OVERCURRENT_MA 15000u
/* Battery protection: low-voltage cutoff, configurable, 11.8V default
 * for LiFePO4. */
#define MC_DIAG_DEFAULT_LV_CUTOFF_MV 11800u
#define MC_DIAG_DEFAULT_LV_CUTOFF_HYSTERESIS_MV 300u
/* Deliberately well above a fully-charged LiFePO4 pack's resting voltage
 * (~13.0-13.3V at rest) so a healthy-but-not-charging battery never reads
 * as "engine running" — automotive/motorcycle charging systems regulate
 * meaningfully higher (commonly ~14.2-14.6V) than any realistic resting
 * reading, giving a real margin rather than a threshold that only works if
 * the resting voltage happens to stay low. */
#define MC_DIAG_DEFAULT_ENGINE_RUN_MV 13800u
#define MC_DIAG_DEFAULT_ENGINE_RUN_HYSTERESIS_MV 300u

/* Per-channel thresholds. */
typedef struct {
    uint16_t open_load_ma;   /* actually energized but current < this => MC_DIAG_FAULT_OPEN_LOAD */
    uint16_t overcurrent_ma; /* current > this => MC_DIAG_FAULT_OVERCURRENT */
} mc_diag_channel_config_t;

/* Persisted as part of mc_config_t (mc_config.h's `diagnostics` field) —
 * these are install-specific choices (what's wired to each channel, what
 * battery chemistry/cutoff the owner wants) worth including in the
 * exportable JSON config backup, unlike board calibration
 * (mc_diag_calib_t) which is NOT part of this struct — see its own doc
 * comment for why. mc_config.c's schema_version was bumped 1->2 when this
 * field was added (see mc_config.h). */
typedef struct {
    mc_diag_channel_config_t channels[MC_OUTPUT_COUNT];

    /* Below this (and only while !engine_running — see mc_diag_tick),
     * mc_diag_tick() engages the low-voltage cutoff (battery protection).
     * Recovers once battery_mv >= lv_cutoff_mv + lv_cutoff_hysteresis_mv. */
    uint16_t lv_cutoff_mv;
    uint16_t lv_cutoff_hysteresis_mv;

    /* Above this, mc_diag_tick() considers the alternator/charging system
     * live and sets engine_running (starter protection). Clears it once
     * battery_mv < engine_run_mv - engine_run_hysteresis_mv. */
    uint16_t engine_run_mv;
    uint16_t engine_run_hysteresis_mv;
} mc_diag_config_t;

void mc_diag_config_default(mc_diag_config_t *out);

/* Config validation, mirroring mc_output_config_validate()'s bitmask style.
 * Pure function; does not touch hardware. */
typedef enum {
    MC_DIAG_CFG_OK = 0,
    /* Some channel's open_load_ma >= overcurrent_ma, so a real fault could
     * never classify as OVERCURRENT (the OPEN_LOAD check runs first and
     * would always win) — not unsafe, just certainly a misconfiguration. */
    MC_DIAG_CFG_BAD_CHANNEL_THRESHOLDS = 1u << 0,
} mc_diag_config_flags_t;

uint32_t mc_diag_config_validate(const mc_diag_config_t *cfg);

/* Board-specific analog calibration — kept OUT of mc_diag_config_t /
 * mc_config_t deliberately: importing board A's gain/offset/kILIS onto
 * board B would silently misreport board B's real current/voltage, unlike
 * every other field in the exportable config. Own NVS blob (mirrors
 * mc_keystore/mc_lock's "own blob, not the JSON config" doctrine) — and
 * unlike a factory reset/ownership transfer (which wipes owner identity),
 * calibration deliberately SURVIVES factory reset: it describes the board
 * itself, not the outgoing owner (see firmware/main/factory_reset.c, which
 * does not erase this namespace). */
typedef struct {
    float is_gain;         /* multiplies (raw IS mV + is_offset_mv); 1.0 = uncalibrated */
    int16_t is_offset_mv;
    /* BTS7008-2EPA datasheet current-sense ratio. Defaults to 1.0, NOT a
     * fabricated "typical" number — the real datasheet value was not
     * available in this development environment, and a fake-precise
     * placeholder would be more misleading than an obviously-unset 1.0.
     * MUST be set from the real datasheet value or bench calibration
     * (MC_OP_DIAG_SET_CALIB) before absolute current readings (not just
     * coarse fault thresholds) are trusted. */
    float kilis;
    float vbat_gain;        /* multiplies (raw VSENSE_BAT mV + vbat_offset_mv) */
    int16_t vbat_offset_mv;
} mc_diag_calib_t;

/* Nominal, uncalibrated defaults: is_gain/vbat_gain = 1.0, kilis = 1.0
 * (deliberately not a fabricated datasheet number — see mc_diag_calib_t),
 * vbat_gain further approximates board Integrated V2's 1MOhm/100kOhm
 * divider (PINOUT.md, ratio 0.0909 => gain ~11.0011 to undo it) as a
 * reasonable starting point; all offsets 0. Recalibrate per board via
 * MC_OP_DIAG_SET_CALIB. */
void mc_diag_calib_default(mc_diag_calib_t *out);

/* Hardware access, injected so this module stays portable — mirrors
 * mc_output_hal_t. firmware/main/diag_hal.c provides the real DSEL/DEN-
 * sequenced ADC read (with its settle delay); firmware/sim provides one
 * that returns injected test/GUI values. */
typedef struct {
    /* One full select+settle+read of channel `channel`'s IS line, in raw
     * millivolts (pre-calibration). Only called for channels mc_diag
     * considers actually energized (mc_output_get_actual_state()). */
    uint16_t (*read_channel_mv)(uint8_t channel, void *ctx);
    /* Raw battery-divider ADC reading, in millivolts (pre-calibration,
     * pre-divider-ratio-correction — that correction lives in
     * mc_diag_calib_t.vbat_gain/vbat_offset_mv). */
    uint16_t (*read_vbat_mv)(void *ctx);
    void *ctx;
} mc_diag_hal_t;

typedef struct {
    mc_diag_config_t config;
    mc_diag_calib_t calib;
    mc_diag_hal_t hal;

    uint16_t current_ma[MC_OUTPUT_COUNT]; /* 0 for any channel not actually energized */
    mc_diag_fault_t fault[MC_OUTPUT_COUNT];
    uint16_t fault_mask; /* bit c set <=> fault[c] != MC_DIAG_FAULT_NONE */

    uint16_t battery_mv;
    bool engine_running;   /* mirrors what mc_diag_tick last pushed to mc_output */
    bool lv_cutoff_active;  /* mirrors what mc_diag_tick last pushed to mc_output */

    uint8_t next_sample_channel; /* round-robin cursor into 0..MC_OUTPUT_COUNT-1 */
} mc_diag_t;

void mc_diag_init(mc_diag_t *diag, const mc_diag_config_t *config, const mc_diag_calib_t *calib, mc_diag_hal_t hal);

/* Call every ~10ms. Samples one ON channel's current per call (PINOUT.md:
 * only one PROFET DEN may be high at a time, so a full 12-channel sweep
 * takes several ticks — current-sense doesn't need to be single-tick-fresh),
 * classifies it against the configured thresholds, reads the battery,
 * derives engine_running and the low-voltage cutoff (each with its own
 * hysteresis band), and pushes both into `output` via
 * mc_output_set_engine_running() / mc_output_set_lv_cutoff(). */
void mc_diag_tick(mc_diag_t *diag, mc_output_engine_t *output, uint32_t now_ms);

/* MC_OP_DIAG_LEARN: samples the given channel's current now (it must be
 * actually energized — mc_output_get_actual_state() — otherwise this
 * channel contributes nothing and, if it's the only one requested, this
 * returns false) and sets its open_load_ma to roughly half the measured
 * healthy draw, so a subsequent real fault (bulb blows, connector opens)
 * still trips well above zero but comfortably below a healthy reading.
 * channel == 0xFF learns every currently-energized channel; returns false
 * only if not a single channel was sampled. Does not touch overcurrent_ma
 * (that stays a manual/config value — a single healthy-current sample says
 * nothing about a safe upper bound). */
bool mc_diag_learn(mc_diag_t *diag, const mc_output_engine_t *output, uint8_t channel);

/* MC_OP_DIAG_SET_CONFIG: applies pre-validated (mc_diag_config_validate)
 * thresholds/cutoff/engine-run settings. Caller is responsible for also
 * updating the persisted copy in mc_config_t.diagnostics (mc_diag_t's copy
 * here is the live/runtime one — same double-bookkeeping mc_output_engine_t
 * and mc_config_t.outputs already do). */
void mc_diag_apply_config(mc_diag_t *diag, const mc_diag_config_t *config);

/* MC_OP_DIAG_SET_CALIB: applies new board calibration constants directly —
 * no validation beyond struct shape, these are installer/bench values,
 * trusted like the rest of the authed command surface. */
void mc_diag_apply_calib(mc_diag_t *diag, const mc_diag_calib_t *calib);

static inline uint16_t mc_diag_get_fault_mask(const mc_diag_t *diag)
{
    return diag->fault_mask;
}
static inline uint16_t mc_diag_get_battery_mv(const mc_diag_t *diag)
{
    return diag->battery_mv;
}

/* --- persistence: mc_diag_calib_t's own NVS blob, versioned envelope like
 * mc_keystore/mc_lock. mc_diag_config_t itself has NO separate serialize
 * here — it rides mc_config_t's existing binary (de)serialize as the
 * `diagnostics` field, same as mc_output_config_t/mc_input_config_t. --- */

typedef enum {
    MC_DIAG_STORE_OK = 0,
    MC_DIAG_STORE_ERR_BUFFER_TOO_SMALL,
    MC_DIAG_STORE_ERR_CORRUPT,
    MC_DIAG_STORE_ERR_FUTURE_VERSION,
} mc_diag_store_result_t;

mc_diag_store_result_t mc_diag_calib_serialize(const mc_diag_calib_t *calib, uint8_t *buf, size_t buf_len, size_t *out_len);
mc_diag_store_result_t mc_diag_calib_deserialize(const uint8_t *buf, size_t len, mc_diag_calib_t *out);
