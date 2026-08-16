#pragma once

/*
 * mc_status — the live status snapshot delivered over the status channel
 * (read + notify). Compact fixed 16-byte little-endian wire layout so it
 * fits in a single BLE notification and is trivial for third-party clients
 * to parse (see docs/PROTOCOL.md).
 *
 * Firmware version, uptime, and output state are always populated.
 * lock_state and byte 15 bit 0 (cheatcode_backoff) are populated by
 * mc_lock. battery_mv and output_fault_mask are populated by
 * mc_diag (mc_session.c's build_status() overlays both from
 * app->diag, the same pattern used for lock_state from app->lock) — they
 * report 0 only when no mc_diag is attached (e.g. a host test's mc_app_t).
 */

#include "mc_types.h"

#define MC_FW_VERSION_MAJOR 0
#define MC_FW_VERSION_MINOR 8
#define MC_FW_VERSION_PATCH 2

#define MC_STATUS_WIRE_LEN 16

/* mc_lock maps its internal state to these wire values via
 * mc_lock_wire_state(). MC_LOCK_UNKNOWN is only reported before mc_lock_init
 * runs at boot, or when a host test's mc_app_t has no lock attached. */
typedef enum {
    MC_LOCK_UNKNOWN = 0,
    MC_LOCK_PARKED = 1,
    MC_LOCK_LOCKED = 2,
    MC_LOCK_UNLOCKED = 3,
} mc_lock_state_t;

typedef struct {
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
    uint8_t lock_state;          /* mc_lock_state_t */
    uint32_t uptime_ms;
    uint16_t battery_mv;         /* mc_diag's calibrated battery reading */
    uint16_t output_state_mask;  /* bit c set => channel c is ON */
    uint16_t output_fault_mask;  /* bit c set => channel c faulted (mc_diag_fault_t) */
    int8_t rssi_dbm;             /* filled by the transport adapter; 0 if unknown */
    bool cheatcode_backoff;      /* wire byte 15 bit 0: cheat-code entry is in backoff */
    bool lv_cutoff_active;       /* wire byte 15 bit 1: low-voltage cutoff is suppressing
                                   * non-essential outputs — see mc_output_lv_cutoff_active(). */
    /* Wire byte 15 bit 2: hazards are running. Deliberately its own bit
     * rather than something a client derives from output_state_mask: hazard
     * members BLINK, so that mask alternates several times a second and a
     * client sampling it can never tell "hazards on" from "hazards off". A
     * rider needs to know whether pressing the button will start or stop
     * them, which is a question about intent, not about what is lit this
     * instant. */
    bool hazard_active;
} mc_status_t;

/* Fills `out` with firmware version + defaults (everything else zeroed,
 * lock_state = MC_LOCK_UNKNOWN). */
void mc_status_init(mc_status_t *out);

/* Serializes to the fixed MC_STATUS_WIRE_LEN little-endian layout. Returns
 * false if buf_len < MC_STATUS_WIRE_LEN. */
bool mc_status_serialize(const mc_status_t *st, uint8_t *buf, size_t buf_len);

/* Parses the fixed wire layout. Returns false if len < MC_STATUS_WIRE_LEN.
 * Provided so host tests and clients can round-trip. */
bool mc_status_deserialize(const uint8_t *buf, size_t len, mc_status_t *out);
