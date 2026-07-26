#pragma once

/*
 * mc_event_log — a persisted, fixed-size ring buffer of security/safety-
 * relevant events (docs/PROTOCOL.md §15). Portable C99, no
 * ESP-IDF dependency — storage goes through an injected HAL, exactly like
 * every other persisted module in this component.
 *
 * Scope is deliberately narrow (AGENTS.md's 7 safety requirements, not
 * routine operation): lock state transitions, key enroll/revoke/ownership-
 * transfer, factory reset, cheat-code lockout, OTA begin/success/failure,
 * low-voltage cutoff enter/exit. NOT routine output toggles or diagnostics
 * faults — those are already visible live via the STATUS/DIAG wire ops;
 * logging every turn-signal blink would fill the ring with noise instead of
 * signal.
 *
 * Unrelated to firmware/sim/src/sim_protocol.h's SIM_CH_DEBUG /
 * SIM_OP_GET_LOG / SIM_OP_LOG_ENTRY — that is a sim-only, in-memory,
 * free-text debug trace for the fault-injection GUI, never reachable from
 * real hardware or this app (see that header's own doc comment). This
 * module is the real, on-device, persisted log; do not confuse the two or
 * reuse that wire format.
 *
 * Records are fixed-size (12 bytes) and structured (an event-type enum +
 * two small argument bytes), not free text — a better fit for a
 * flash-constrained device (4MB total, no PSRAM) than the sim debug log's
 * printf-style strings.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MC_EVT_NONE = 0,
    MC_EVT_LOCK_ENGAGED = 1,          /* arg0 unused */
    MC_EVT_LOCK_RELEASED = 2,         /* arg0: mc_event_unlock_method_t */
    MC_EVT_KEY_ENROLLED = 3,          /* arg0: keystore slot */
    MC_EVT_KEY_REVOKED = 4,           /* arg0: keystore slot */
    MC_EVT_OWNERSHIP_TRANSFERRED = 5, /* arg0 unused */
    MC_EVT_FACTORY_RESET = 6,         /* arg0: 0 = physical BOOT-hold */
    MC_EVT_CHEATCODE_LOCKOUT = 7,     /* arg0: consecutive_wrong count at trip (capped at 255) */
    MC_EVT_OTA_BEGIN = 8,             /* arg0 unused */
    MC_EVT_OTA_SUCCESS = 9,           /* arg0 unused */
    MC_EVT_OTA_FAILURE = 10,          /* arg0: mc_ota_result_t */
    MC_EVT_LV_CUTOFF_ENTER = 11,      /* arg0 unused */
    MC_EVT_LV_CUTOFF_EXIT = 12,       /* arg0 unused */
} mc_event_type_t;

/* arg0 values for MC_EVT_LOCK_RELEASED — which unlock method released it. */
typedef enum {
    MC_EVT_UNLOCK_PHONE_AUTO = 0,
    MC_EVT_UNLOCK_EXPLICIT = 1,
    MC_EVT_UNLOCK_CHEATCODE = 2,
    MC_EVT_UNLOCK_IGNITION_SWITCH = 3,
    MC_EVT_UNLOCK_TRANSFER_OR_RESET = 4,
} mc_event_unlock_method_t;

#define MC_EVENT_LOG_RECORD_BYTES 12

typedef struct {
    uint32_t seq;       /* 1-based, monotonic, never reused; 0 = empty/unwritten slot */
    uint32_t uptime_ms; /* device uptime at the event -- no RTC on this hardware */
    uint8_t type;        /* mc_event_type_t */
    uint8_t arg0;
    uint8_t arg1;        /* reserved for future use; always 0 today */
    uint8_t reserved;    /* must be 0 */
} mc_event_record_t;

#define MC_EVENT_LOG_SLOT_COUNT 1024

/* Storage HAL, injected so this module stays portable. On real hardware
 * (firmware/main/nvs_event_log_hal.c) this wraps a dedicated NVS partition
 * ("evtlog", firmware/partitions.csv) so the log's erase-cycle wear is
 * isolated from the config/keystore/lock/calib blobs sharing the main 32KB
 * `nvs` partition. */
typedef struct {
    bool (*read_slot)(uint16_t slot, mc_event_record_t *out, void *ctx); /* false = never written / not found */
    bool (*write_slot)(uint16_t slot, const mc_event_record_t *rec, void *ctx);
    uint32_t (*get_last_seq)(void *ctx); /* 0 = empty log */
    bool (*set_last_seq)(uint32_t seq, void *ctx);
    void *ctx;
} mc_event_log_hal_t;

typedef struct {
    mc_event_log_hal_t hal;
    uint32_t last_seq;
} mc_event_log_t;

/* Loads last_seq from the HAL (0 if empty/unavailable). O(1) — no
 * full-partition scan needed at boot. */
void mc_event_log_init(mc_event_log_t *log, mc_event_log_hal_t hal);

/* Appends one record, timestamped uptime_ms. A HAL write failure is
 * swallowed silently — logging must never become a fatal path for the
 * operation it's logging (same doctrine as every other persist_* hook in
 * this codebase). Once MC_EVENT_LOG_SLOT_COUNT records have been written,
 * each new append silently evicts the oldest (ring buffer). */
void mc_event_log_append(mc_event_log_t *log, mc_event_type_t type, uint8_t arg0, uint8_t arg1, uint32_t now_ms);

/* Number of records with seq > since_seq currently available (i.e. what
 * mc_event_log_read(since_seq, ..., SIZE_MAX) would return), without
 * materializing any of them — lets a chunked-transfer caller (mc_session.c)
 * compute the wire "total" field up front. */
size_t mc_event_log_count_since(const mc_event_log_t *log, uint32_t since_seq);

/* Reads up to max_count records with seq > since_seq, oldest-first, into
 * `out`. Returns the count actually written. Automatically skips any
 * evicted (seq <= last_seq - SLOT_COUNT) or corrupt/never-written slot
 * (detected by the slot's own stored seq not matching the expected value —
 * doubles as corruption detection, no separate checksum field needed). */
size_t mc_event_log_read(const mc_event_log_t *log, uint32_t since_seq, mc_event_record_t *out, size_t max_count);

/* Resets last_seq to 0 (logically empties the log — old slots are simply
 * no longer reachable by mc_event_log_read, since every seq <= the reset
 * cursor's neighborhood looks "evicted"; they are not explicitly
 * overwritten). Used only internally by ownership-transfer and factory
 * reset (mc_session.c / factory_reset.c), never as a standalone
 * client-invokable wire command. */
void mc_event_log_clear(mc_event_log_t *log);

static inline uint32_t mc_event_log_last_seq(const mc_event_log_t *log)
{
    return log->last_seq;
}
