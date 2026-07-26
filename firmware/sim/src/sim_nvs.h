#pragma once

/*
 * sim_nvs — a fake NVS blob store for the host simulator.
 *
 * Real hardware persists mc_config / mc_keystore blobs via ESP-IDF NVS
 * (firmware/main/nvs_config_hal.c, nvs_keystore_hal.c). The simulator has no
 * flash, so this stands in: two in-memory byte buffers, written through
 * mc_config_save()/mc_keystore_serialize() the same way the real HALs do,
 * and read back the same way on a simulated reboot. This lets the
 * "simulated NVS corruption" test exercise the REAL mc_config_load()
 * (mc_config_from_json() under the hood) / mc_keystore_deserialize()
 * fallback-to-defaults logic, not a fake stand-in for it.
 */

#include "mc_config.h"
#include "mc_config_json.h"
#include "mc_diag.h"
#include "mc_event_log.h"
#include "mc_keystore.h"
#include "mc_lock.h"

/* Config's on-device persistence format is JSON
 * (MC_CONFIG_JSON_MAX, mc_config_json.h), not a raw struct memcpy — this
 * must be at least that large, since the config slot is where the sim
 * stores it. */
#define SIM_NVS_BUF_MAX MC_CONFIG_JSON_MAX

typedef struct {
    uint8_t buf[SIM_NVS_BUF_MAX];
    size_t len;
    bool present;
} sim_nvs_slot_t;

/* Fake storage for the event log's dedicated NVS partition
 * ("evtlog" on real hardware, firmware/main/nvs_event_log_hal.c) — a fixed
 * array of slots plus a cursor, not a single blob like the others above,
 * mirroring the real partition's per-record-key layout. */
typedef struct {
    mc_event_record_t records[MC_EVENT_LOG_SLOT_COUNT];
    bool record_present[MC_EVENT_LOG_SLOT_COUNT];
    uint32_t last_seq;
} sim_nvs_evtlog_t;

typedef struct {
    sim_nvs_slot_t config;
    sim_nvs_slot_t keystore;
    sim_nvs_slot_t lock;
    sim_nvs_slot_t calib; /* Board calibration blob, mirrors .lock */
    sim_nvs_evtlog_t evtlog;
} sim_nvs_t;

void sim_nvs_init(sim_nvs_t *nvs);

/* mc_config_store_hal_t-compatible load/save, backed by nvs->config. */
mc_config_result_t sim_nvs_config_load(uint8_t *buf, size_t buf_len, size_t *out_len, void *ctx);
mc_config_result_t sim_nvs_config_save(const uint8_t *buf, size_t len, void *ctx);

/* Keystore has no generic store_hal_t in mc_keystore.h (it's persisted via a
 * hand-rolled HAL on-target too), so these call mc_keystore_serialize /
 * deserialize directly against nvs->keystore. */
bool sim_nvs_keystore_load(sim_nvs_t *nvs, mc_keystore_t *out);
bool sim_nvs_keystore_save(sim_nvs_t *nvs, const mc_keystore_t *ks);

/* Same pattern for the lock blob (mc_lock_serialize/deserialize), against
 * nvs->lock. If nothing is stored, `out_config` gets immobilizer-disabled
 * defaults and `out_locked_flag` false — a fresh/factory-reset device boots
 * unlocked, same as the real nvs_lock_hal. */
bool sim_nvs_lock_load(sim_nvs_t *nvs, mc_lock_config_t *out_config, bool *out_locked_flag);
bool sim_nvs_lock_save(sim_nvs_t *nvs, const mc_lock_t *lock);

/* Same pattern for the diagnostics calibration blob (mc_diag_calib_serialize/
 * deserialize), against nvs->calib. If nothing is stored, `out` gets the
 * SIM'S OWN identity calibration (is_gain=1, kilis=2000, vbat_gain=1, all
 * offsets 0) rather than mc_diag_calib_default()'s real-hardware nominal
 * values — deliberately, so SIM_OP_SET_BATTERY_MV / SIM_OP_SET_CHANNEL_FAULT
 * injected values pass through mc_diag's real current/voltage formula
 * completely unchanged by default (mc_diag.h: with kilis=2000 and unity
 * gain/zero offset, calc_current_ma(mv) == mv and calc_battery_mv(mv) == mv
 * exactly). This is only the "nothing calibrated yet" starting point — a
 * test that explicitly calls MC_OP_DIAG_SET_CALIB still changes the reported
 * values, exactly like real hardware would. */
bool sim_nvs_calib_load(sim_nvs_t *nvs, mc_diag_calib_t *out);
bool sim_nvs_calib_save(sim_nvs_t *nvs, const mc_diag_calib_t *calib);

/* mc_event_log_hal_t-compatible functions, backed by nvs->evtlog. `ctx`
 * must be the owning sim_nvs_t* (matches how mc_event_log_init() is wired:
 * hal.ctx = &nvs, not &nvs.evtlog, so a single sim_nvs_t can back every
 * store through one consistent ctx convention). */
bool sim_nvs_evtlog_read_slot(uint16_t slot, mc_event_record_t *out, void *ctx);
bool sim_nvs_evtlog_write_slot(uint16_t slot, const mc_event_record_t *rec, void *ctx);
uint32_t sim_nvs_evtlog_get_last_seq(void *ctx);
bool sim_nvs_evtlog_set_last_seq(uint32_t seq, void *ctx);

/* Simulates a factory reset / ownership transfer wiping the evtlog
 * partition (mirrors nvs_flash_erase_partition on real hardware). */
void sim_nvs_evtlog_erase(sim_nvs_t *nvs);

/* Flips bytes in the stored blob to simulate flash/NVS corruption. Has no
 * effect until the next load (i.e. the next simulated reboot) — mirrors the
 * real failure mode, where corruption is only discovered on read. */
void sim_nvs_corrupt(sim_nvs_slot_t *slot);

/* Erases a slot entirely (simulates a blank/never-written NVS entry, i.e.
 * MC_CONFIG_ERR_NOT_FOUND on next load). */
void sim_nvs_erase(sim_nvs_slot_t *slot);
