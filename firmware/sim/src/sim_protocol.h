#pragma once

/*
 * sim_protocol — the debug/fault-injection channel used ONLY by the
 * simulator debug GUI (firmware/sim/gui/), never sent over real BLE and
 * never referenced by firmware/components/core or docs/PROTOCOL.md.
 *
 * This is deliberately a separate header from mc_protocol.h: it exists so
 * a pre-hardware test harness can drive things no real client ever could
 * (fake sensor values, forced faults) without adding sim-only concepts to
 * the protocol third-party clients implement against. See docs/TESTING.md.
 *
 * Framing matches the real protocol for consistency: on the WebSocket
 * transport, [channel byte][opcode][payload]. SIM_CH_DEBUG (0x7F) is chosen
 * well clear of the real mc_channel_t range (0..MC_CH_COUNT-1 = 0..4) so a
 * real client (or docs/PROTOCOL.md reader) can never confuse the two.
 * Device->client opcodes have bit 0x80 set, matching mc_protocol.h's
 * convention.
 */

#include "mc_types.h"

#define SIM_CH_DEBUG 0x7F

typedef enum {
    SIM_FAULT_NONE = 0,
    SIM_FAULT_OPEN_LOAD = 1,
    SIM_FAULT_OVERCURRENT = 2,
    SIM_FAULT_SHORT = 3,
} sim_fault_t;

typedef enum {
    SIM_NVS_TARGET_CONFIG = 0,
    SIM_NVS_TARGET_KEYSTORE = 1,
    SIM_NVS_TARGET_BOTH = 2,   /* config + keystore only, for backward compatibility */
    SIM_NVS_TARGET_LOCK = 3,  /* The lock/cheat-code blob, corrupted independently */
    SIM_NVS_TARGET_CALIB = 4, /* The board calibration blob, corrupted independently */
} sim_nvs_target_t;

/* --- client -> sim --- */
#define SIM_OP_SET_BATTERY_MV 0x01     /* [mv:u16le] */
/* [channel:1][current_ma:u16le][fault:1] -- `fault` is
 * IGNORED (the real mc_diag classifies from current_ma against the real,
 * configured thresholds; forcing a fault byte independent of current_ma
 * would let the sim show a fault mc_diag itself never derived). Kept in the
 * wire shape only for backward compatibility with older recorded GUI
 * scenarios (see docs/TESTING.md). Send a current_ma below/above the
 * channel's configured thresholds to trigger a real fault. */
#define SIM_OP_SET_CHANNEL_FAULT 0x02
/* [running:1] -- mc_diag derives engine_running for real from
 * injected battery voltage every tick (starter protection); this op no longer
 * sets the flag directly (it would just be overwritten by the very next
 * tick). Instead it nudges the injected battery voltage to a value that
 * makes the real derivation land on the requested state, so the op stays
 * meaningful without a second, competing "engine_running" concept — see
 * sim_debug.c. */
#define SIM_OP_SET_ENGINE_RUNNING 0x03
#define SIM_OP_SET_INTERLOCK 0x04      /* [engaged:1] */
#define SIM_OP_BUTTON_STATE 0x05       /* [button:1][pressed:1] */
#define SIM_OP_FORCE_DISCONNECT 0x06   /* (no payload) closes the socket */
#define SIM_OP_FORCE_REBOOT 0x07       /* (no payload) reload from fake NVS, then close the socket */
#define SIM_OP_FORCE_NVS_CORRUPT 0x08  /* [target:1] (sim_nvs_target_t) */
#define SIM_OP_GET_LOG 0x09            /* (no payload) replays the event log ring buffer */
#define SIM_OP_GET_STATE 0x0A          /* (no payload) request a SIM_OP_STATE snapshot */
#define SIM_OP_RESET_FAULTS 0x0B       /* (no payload) clear injected current/fault/battery override */

/* --- sim -> client --- */
#define SIM_OP_ACK 0x81      /* [req_op:1][ok:1] */
#define SIM_OP_LOG_ENTRY 0x90/* [t_ms:u32le][text_len:1][text bytes], pushed async + replayed on GET_LOG */
/* [battery_mv:u16le][engine_running:1][interlock:1]
 * then MC_OUTPUT_COUNT * [current_ma:u16le][fault:1]
 * then [lock_state:1 (mc_lock_state_t)][cheatcode_backoff:1]
 * battery_mv, engine_running, current_ma, and fault are the
 * REAL mc_diag-computed values (round-tripped through its calibration and
 * threshold logic), not raw injected numbers — see SIM_OP_SET_CHANNEL_FAULT
 * and SIM_OP_SET_ENGINE_RUNNING above. */
#define SIM_OP_STATE 0x91

/* Note: there is no separate debug op to simulate the
 * ignition-switch input — it isn't a distinct simulated signal, it's one of
 * the same 8 buttons (mc_lock_config_t.ignition_switch_input is an input
 * index, same as starter_interlock_input). Configure that input via the
 * real protocol's MC_OP_LOCK_SET_CONFIG, then drive it with the existing
 * SIM_OP_BUTTON_STATE — held (pressed=1) simulates the switch ON, released
 * simulates it OFF, exactly like a real maintained switch's debounced
 * level. */

/* Note: there is no separate debug op for the IS mux / DSEL / DEN
 * sequencing either — that's real-hardware plumbing with no simulated
 * equivalent. The sim's mc_diag_hal_t reads injected values directly (see
 * firmware/sim/src/main.c); SIM_OP_SET_CHANNEL_FAULT / SIM_OP_SET_BATTERY_MV
 * are the injection points, and the REAL mc_diag threshold/calibration/
 * cutoff/engine-running logic runs on top of them, unmodified from what
 * ships on-target. */

#define SIM_LOG_TEXT_MAX 96
#define SIM_LOG_RING_LEN 128
