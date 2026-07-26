#pragma once

/*
 * sim_debug — handler for the sim-only debug channel (sim_protocol.h) that
 * backs the browser GUI: fake battery/current/fault injection,
 * engine-running/interlock toggles for exercising starter protection ahead
 * of real hardware sensing, forced disconnect/reboot/NVS-corruption, and the
 * live event log.
 *
 * Owns nothing by allocation — `sim_debug_ctx_t` just points at the same
 * engine/config/keystore/nvs/persist-scheduler instances firmware/sim/src/
 * main.c already owns, so this module can mutate live device state (e.g.
 * simulate a reboot by reloading config from the fake NVS and re-running
 * mc_output_restore_from_config) the same way a real reboot would.
 *
 * engine_running is not separately tracked injection state
 * here — mc_diag derives it for real from ctx->battery_mv every tick and
 * pushes it into ctx->output directly (see sim_debug.c's SIM_OP_STATE
 * handler, which now reads ctx->output->engine_running as the single
 * source of truth instead of a parallel ctx-local flag).
 *
 * Thread safety: every field is protected by `lock`. main.c must hold this
 * lock around any access to g_output/g_config/g_keystore/g_input from
 * *either* the WebSocket read thread (handling a real protocol message) or
 * the ticker thread (input polling, persistence flush) — sim_debug_lock() /
 * sim_debug_unlock() are the shared entry points for both.
 */

#include <pthread.h>

#include "mc_config.h"
#include "mc_diag.h"
#include "mc_input.h"
#include "mc_keystore.h"
#include "mc_lock.h"
#include "mc_output.h"
#include "mc_persist.h"
#include "sim_nvs.h"
#include "sim_protocol.h"

typedef struct {
    mc_output_engine_t *output;
    mc_input_engine_t *input;
    mc_config_t *config;
    mc_keystore_t *keystore;
    mc_lock_t *mc_lock;
    mc_diag_t *diag;
    sim_nvs_t *nvs;
    mc_persist_scheduler_t *persist_config;
    mc_persist_scheduler_t *persist_keystore;
    uint32_t (*now_ms)(void);

    pthread_mutex_t lock;

    /* Fault-injection state: the raw values mc_diag's sim HAL reads back
     * (see firmware/sim/src/main.c) — real mc_diag threshold/calibration
     * logic runs on top of these, so `channel_fault` below is no longer the
     * source of truth for the reported fault (mc_diag->fault[] is; see
     * SIM_OP_SET_CHANNEL_FAULT's doc comment in sim_protocol.h). */
    uint16_t battery_mv;
    bool interlock_engaged;
    uint16_t channel_current_ma[MC_OUTPUT_COUNT];
    sim_fault_t channel_fault[MC_OUTPUT_COUNT]; /* legacy injection field, kept for wire compat only */

    /* Virtual button state, sampled by the ticker's mc_input_poll() call. */
    bool button_pressed[MC_INPUT_COUNT];
} sim_debug_ctx_t;

void sim_debug_init(sim_debug_ctx_t *ctx,
                    mc_output_engine_t *output, mc_input_engine_t *input,
                    mc_config_t *config, mc_keystore_t *keystore, mc_lock_t *lock, mc_diag_t *diag, sim_nvs_t *nvs,
                    mc_persist_scheduler_t *persist_config, mc_persist_scheduler_t *persist_keystore,
                    uint32_t (*now_ms)(void));

void sim_debug_lock(sim_debug_ctx_t *ctx);
void sim_debug_unlock(sim_debug_ctx_t *ctx);

/* Appends a printf-style entry to the event log ring buffer, echoes it to
 * stdout, and pushes it live to the connected GUI (if any) as
 * SIM_OP_LOG_ENTRY. Safe to call with ctx->lock already held. */
void sim_debug_logf(sim_debug_ctx_t *ctx, const char *fmt, ...);

/* Dispatches one SIM_CH_DEBUG frame (payload = opcode byte + body, same
 * convention as mc_session_handle). Acquires/releases ctx->lock itself. */
void sim_debug_handle(sim_debug_ctx_t *ctx, const uint8_t *payload, size_t len);

/* Populates a snapshot from the current fault-injection state; caller must
 * hold ctx->lock (mc_session's status handler already runs under the shared
 * lock in main.c). */
void sim_debug_fill_status_extras(sim_debug_ctx_t *ctx, uint16_t *out_battery_mv, uint16_t *out_fault_mask);
