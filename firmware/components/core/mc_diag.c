#include "mc_diag.h"

#include <string.h>

/* --- calibration math (hardware/PINOUT.md "Diagnostics readout procedure") --- */

static uint16_t calc_current_ma(const mc_diag_calib_t *calib, uint16_t raw_mv)
{
    float mv = ((float)raw_mv + (float)calib->is_offset_mv) * calib->is_gain;
    if (mv < 0.0f) {
        mv = 0.0f;
    }
    /* I[mA] = (V_IS[mV] / 2000 Ohm) * kILIS -- R3 is the 2kOhm sense resistor. */
    float ma = (mv / 2000.0f) * calib->kilis;
    if (ma < 0.0f) {
        ma = 0.0f;
    }
    if (ma > 65535.0f) {
        ma = 65535.0f;
    }
    return (uint16_t)(ma + 0.5f);
}

static uint16_t calc_battery_mv(const mc_diag_calib_t *calib, uint16_t raw_mv)
{
    float mv = ((float)raw_mv + (float)calib->vbat_offset_mv) * calib->vbat_gain;
    if (mv < 0.0f) {
        mv = 0.0f;
    }
    if (mv > 65535.0f) {
        mv = 65535.0f;
    }
    return (uint16_t)(mv + 0.5f);
}

/* --- config / calib defaults --- */

void mc_diag_config_default(mc_diag_config_t *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        out->channels[i].open_load_ma = MC_DIAG_DEFAULT_OPEN_LOAD_MA;
        out->channels[i].overcurrent_ma = MC_DIAG_DEFAULT_OVERCURRENT_MA;
    }
    out->lv_cutoff_mv = MC_DIAG_DEFAULT_LV_CUTOFF_MV;
    out->lv_cutoff_hysteresis_mv = MC_DIAG_DEFAULT_LV_CUTOFF_HYSTERESIS_MV;
    out->engine_run_mv = MC_DIAG_DEFAULT_ENGINE_RUN_MV;
    out->engine_run_hysteresis_mv = MC_DIAG_DEFAULT_ENGINE_RUN_HYSTERESIS_MV;
}

uint32_t mc_diag_config_validate(const mc_diag_config_t *cfg)
{
    uint32_t flags = 0;
    for (int i = 0; i < MC_OUTPUT_COUNT; i++) {
        if (cfg->channels[i].open_load_ma >= cfg->channels[i].overcurrent_ma) {
            flags |= MC_DIAG_CFG_BAD_CHANNEL_THRESHOLDS;
        }
    }
    return flags;
}

void mc_diag_calib_default(mc_diag_calib_t *out)
{
    memset(out, 0, sizeof(*out));
    out->is_gain = 1.0f;
    out->is_offset_mv = 0;
    out->kilis = 1.0f; /* deliberately not a fabricated datasheet number -- see mc_diag.h */
    out->vbat_gain = 11.0011f; /* 1 / 0.0909 -- board Integrated V2's 1MOhm/100kOhm divider, PINOUT.md */
    out->vbat_offset_mv = 0;
}

/* --- lifecycle --- */

void mc_diag_init(mc_diag_t *diag, const mc_diag_config_t *config, const mc_diag_calib_t *calib, mc_diag_hal_t hal)
{
    memset(diag, 0, sizeof(*diag));
    diag->config = *config;
    diag->calib = *calib;
    diag->hal = hal;
}

void mc_diag_tick(mc_diag_t *diag, mc_output_engine_t *output, uint32_t now_ms)
{
    /* The low-voltage/engine-running hysteresis below is value-based (a gap
     * between engage/disengage thresholds), not time-based. now_ms is used
     * for mc_output_get_actual_state()'s blink-phase-accurate check:
     * a MC_OUT_BEHAVIOUR_BLINK channel's off-phase must read as
     * "not energized" here, or its every-other-tick dark half would get
     * misclassified as an open-load fault. */

    /* --- round-robin current sampling: sample at most one real (actually
     * energized) channel per call. Channels that aren't actually energized
     * (commanded off, mid-blink-off, or suppressed by the low-voltage
     * cutoff) report 0 and are never fault-classified -- current-sense is
     * only meaningful for a channel that's actually driving a load right
     * now. */
    for (uint8_t i = 0; i < MC_OUTPUT_COUNT; i++) {
        uint8_t ch = diag->next_sample_channel;
        diag->next_sample_channel = (uint8_t)((diag->next_sample_channel + 1) % MC_OUTPUT_COUNT);

        if (!mc_output_get_actual_state(output, ch, now_ms)) {
            diag->current_ma[ch] = 0;
            diag->fault[ch] = MC_DIAG_FAULT_NONE;
            continue; /* keep scanning this same tick until an energized channel is found */
        }

        uint16_t raw_mv = (diag->hal.read_channel_mv != NULL) ? diag->hal.read_channel_mv(ch, diag->hal.ctx) : 0;
        diag->current_ma[ch] = calc_current_ma(&diag->calib, raw_mv);

        const mc_diag_channel_config_t *th = &diag->config.channels[ch];
        if (diag->current_ma[ch] < th->open_load_ma) {
            diag->fault[ch] = MC_DIAG_FAULT_OPEN_LOAD;
        } else if (diag->current_ma[ch] > th->overcurrent_ma) {
            diag->fault[ch] = MC_DIAG_FAULT_OVERCURRENT;
        } else {
            diag->fault[ch] = MC_DIAG_FAULT_NONE;
        }
        break; /* one real sample per tick; the rest wait for their turn */
    }

    uint16_t mask = 0;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (diag->fault[ch] != MC_DIAG_FAULT_NONE) {
            mask |= (uint16_t)(1u << ch);
        }
    }
    diag->fault_mask = mask;

    /* --- battery + derived signals --- */
    uint16_t raw_vbat_mv = (diag->hal.read_vbat_mv != NULL) ? diag->hal.read_vbat_mv(diag->hal.ctx) : 0;
    diag->battery_mv = calc_battery_mv(&diag->calib, raw_vbat_mv);

    bool engine_running = diag->engine_running;
    if (!engine_running && diag->battery_mv >= diag->config.engine_run_mv) {
        engine_running = true;
    } else if (engine_running &&
               diag->battery_mv < (diag->config.engine_run_mv - diag->config.engine_run_hysteresis_mv)) {
        engine_running = false;
    }
    diag->engine_running = engine_running;
    mc_output_set_engine_running(output, engine_running);

    bool cutoff = diag->lv_cutoff_active;
    if (engine_running) {
        /* Charging: the alternator/regulator is holding voltage up, so a
         * stale low reading from just before the engine started must never
         * linger into a cutoff (ride-safe failure: never drop essential outputs
         * mid-ride, and don't even flirt with non-essential ones either). */
        cutoff = false;
    } else if (!cutoff) {
        /* battery_mv == 0 almost certainly means an unread/failed ADC line
         * (or, on the host, a HAL with no read_vbat_mv wired up), not a
         * genuinely dead battery -- treat that as "unknown" rather than
         * tripping the cutoff on a bogus zero. */
        if (diag->battery_mv > 0 && diag->battery_mv < diag->config.lv_cutoff_mv) {
            cutoff = true;
        }
    } else if (diag->battery_mv >= (uint32_t)diag->config.lv_cutoff_mv + diag->config.lv_cutoff_hysteresis_mv) {
        cutoff = false;
    }
    diag->lv_cutoff_active = cutoff;
    mc_output_set_lv_cutoff(output, cutoff);
}

bool mc_diag_learn(mc_diag_t *diag, const mc_output_engine_t *output, uint8_t channel)
{
    if (channel != 0xFFu && channel >= MC_OUTPUT_COUNT) {
        return false;
    }
    bool learned_any = false;
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (channel != 0xFFu && ch != channel) {
            continue;
        }
        if (!mc_output_get_actual_state(output, ch, output->last_tick_ms)) {
            continue;
        }
        uint16_t raw_mv = (diag->hal.read_channel_mv != NULL) ? diag->hal.read_channel_mv(ch, diag->hal.ctx) : 0;
        uint16_t measured_ma = calc_current_ma(&diag->calib, raw_mv);
        diag->config.channels[ch].open_load_ma = (uint16_t)(measured_ma / 2u);
        learned_any = true;
    }
    return learned_any;
}

void mc_diag_apply_config(mc_diag_t *diag, const mc_diag_config_t *config)
{
    diag->config = *config;
}

void mc_diag_apply_calib(mc_diag_t *diag, const mc_diag_calib_t *calib)
{
    diag->calib = *calib;
}

/* --- persistence: calibration blob only --- */

#define MC_DIAG_CALIB_MAGIC 0x3144434Du /* "MCD1" little-endian */
#define MC_DIAG_CALIB_HEADER_LEN 4u
#define MC_DIAG_CALIB_SCHEMA_VERSION 1

mc_diag_store_result_t mc_diag_calib_serialize(const mc_diag_calib_t *calib, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    size_t total = MC_DIAG_CALIB_HEADER_LEN + sizeof(uint16_t) + sizeof(*calib);
    if (buf_len < total) {
        return MC_DIAG_STORE_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t magic = MC_DIAG_CALIB_MAGIC;
    uint16_t version = MC_DIAG_CALIB_SCHEMA_VERSION;
    memcpy(buf, &magic, sizeof(magic));
    memcpy(buf + MC_DIAG_CALIB_HEADER_LEN, &version, sizeof(version));
    memcpy(buf + MC_DIAG_CALIB_HEADER_LEN + sizeof(version), calib, sizeof(*calib));

    *out_len = total;
    return MC_DIAG_STORE_OK;
}

mc_diag_store_result_t mc_diag_calib_deserialize(const uint8_t *buf, size_t len, mc_diag_calib_t *out)
{
    if (len < MC_DIAG_CALIB_HEADER_LEN + sizeof(uint16_t)) {
        return MC_DIAG_STORE_ERR_CORRUPT;
    }

    uint32_t magic;
    memcpy(&magic, buf, sizeof(magic));
    if (magic != MC_DIAG_CALIB_MAGIC) {
        return MC_DIAG_STORE_ERR_CORRUPT;
    }

    uint16_t version;
    memcpy(&version, buf + MC_DIAG_CALIB_HEADER_LEN, sizeof(version));
    if (version > MC_DIAG_CALIB_SCHEMA_VERSION) {
        return MC_DIAG_STORE_ERR_FUTURE_VERSION;
    }

    if (len != MC_DIAG_CALIB_HEADER_LEN + sizeof(version) + sizeof(*out)) {
        return MC_DIAG_STORE_ERR_CORRUPT;
    }
    memcpy(out, buf + MC_DIAG_CALIB_HEADER_LEN + sizeof(version), sizeof(*out));
    return MC_DIAG_STORE_OK;
}
