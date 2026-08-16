#pragma once

/*
 * mc_lock — the immobilizer / lock state machine (AGENTS.md safety
 * requirements #2 and #3).
 *
 * Four states: DISABLED (no immobilizer configured), UNLOCKED (riding/
 * idling), PARKED (stopped, auto-lock grace timer running), LOCKED
 * (immobilized — ignition and starter refused by mc_output). See
 * docs/PROTOCOL.md §11 for the full state diagram and truth table; this
 * header documents the API, not the design rationale.
 *
 * Like every other core module, time is injected (`now_ms`) and this file
 * has no ESP-IDF/FreeRTOS dependency — fully host-testable. It is ticked
 * from the same task that polls mc_input (every ~10ms) and is wired
 * directly to a mc_output_engine_t: it is the only caller of
 * mc_output_set_immobilized(), and reads mc_output_find_ignition_channel()
 * / mc_output_get_state() to know whether ignition is currently live.
 *
 * Three unlock methods compose (never mutually exclusive):
 *   - Phone-as-key: edge-triggered — the platform calls
 *     mc_lock_request_unlock() directly from mc_app_t.on_session_authed
 *     (mc_session.h) exactly when a BLE session's challenge-response newly
 *     succeeds, if the PHONE method bit is set. See
 *     mc_lock_request_unlock()'s doc comment for why this is an edge, not
 *     a level polled by mc_lock_tick().
 *   - Button cheat-code: ALWAYS active whenever the immobilizer is
 *     enabled — it is the mandatory fallback (AGENTS.md #3), never a
 *     toggle. Entered as a sequence of short button presses fed one at a
 *     time via mc_lock_cheatcode_press(). The code itself is never stored
 *     in cleartext (see mc_lock_config_t) or in this struct's RAM beyond
 *     the in-progress entry buffer.
 *   - Ignition-switch mode: a configured input's debounced level, if the
 *     IGNITION_SWITCH method bit is set.
 *
 * Cheat-code storage: a salted SHA-512 hash, in a dedicated blob (NOT part
 * of the exportable JSON config — same doctrine as mc_keystore, AGENTS.md
 * #4). Wrong-entry backoff is RAM-only (mc_lock_t is never partially
 * persisted) and gates only the cheat-code; phone-as-key and the
 * ignition-switch are unaffected by it.
 */

#include "mc_crypto.h"
#include "mc_output.h"
#include "mc_status.h"
#include "mc_types.h"

/* Cheat-code length bounds, matching the combo matcher's own bounds
 * (mc_input.h: "the cheat-code is 4-10 per spec"). */
#define MC_LOCK_CHEATCODE_MIN_LEN 4
#define MC_LOCK_CHEATCODE_MAX_LEN MC_COMBO_MAX_LEN /* 10 */
#define MC_LOCK_SALT_BYTES 16

/* Default auto-lock grace period and cheat-code entry window, used by
 * mc_lock_config_default(). Both are app-configurable. */
#define MC_LOCK_DEFAULT_AUTO_LOCK_GRACE_MS 60000u
#define MC_LOCK_DEFAULT_CHEATCODE_WINDOW_MS 5000u

/* --- config (persisted; NOT part of the exportable JSON config) --- */

typedef struct {
    bool immobilizer_enabled;
    uint8_t methods_mask;              /* MC_LOCK_METHOD_* bits (mc_protocol.h) */
    int8_t ignition_switch_input;      /* 0..MC_INPUT_COUNT-1, or -1 */
    uint32_t auto_lock_grace_ms;
    uint32_t cheatcode_window_ms;

    bool cheatcode_set;
    uint8_t cheatcode_len;             /* 0 if cheatcode_set is false */
    uint8_t cheatcode_salt[MC_LOCK_SALT_BYTES];
    uint8_t cheatcode_hash[MC_CRYPTO_HASH_BYTES];
} mc_lock_config_t;

/* Config validation problems, mirroring mc_output_config_validate()'s
 * bitmask style. `outputs` is the device's current output config, needed
 * to check "an ignition channel exists" — there is nothing to immobilize
 * without one. */
typedef enum {
    MC_LOCK_CFG_OK = 0,
    /* AGENTS.md #3: the immobilizer may not be enabled with the phone as the
     * only way in. At least one non-phone method must be configured — the
     * button cheat-code OR an ignition-switch input.
     *
     * Was ENABLE_REQUIRES_CHEATCODE, i.e. the code specifically. A rider with
     * an OEM key switch wired to an input already has a physical fallback, and
     * making them also set a code they will never use bought no safety. The
     * invariant that matters — never only the phone — is unchanged. */
    MC_LOCK_CFG_ENABLE_REQUIRES_FALLBACK = 1u << 0,
    MC_LOCK_CFG_ENABLE_REQUIRES_IGNITION_CHANNEL = 1u << 1,
    MC_LOCK_CFG_BAD_IGNITION_SWITCH_INPUT = 1u << 2,
} mc_lock_config_flags_t;

void mc_lock_config_default(mc_lock_config_t *out);
uint32_t mc_lock_config_validate(const mc_lock_config_t *cfg, const mc_output_config_t *outputs);

/* --- live state (RAM; the persisted subset is {config, locked_flag}) --- */

typedef enum {
    MC_LOCK_ST_DISABLED = 0,
    MC_LOCK_ST_UNLOCKED,
    MC_LOCK_ST_PARKED,
    MC_LOCK_ST_LOCKED,
} mc_lock_internal_state_t;

typedef struct {
    mc_lock_config_t config;
    mc_lock_internal_state_t state;
    bool locked_flag;          /* persisted: restore LOCKED on boot, subject to the ride-safe override */
    uint32_t parked_since_ms;  /* origin of the auto-lock grace timer */

    /* In-progress cheat-code entry. RAM only — never persisted, never
     * survives a reboot (matches mc_input's own combo-progress state). */
    uint8_t entry_buf[MC_LOCK_CHEATCODE_MAX_LEN];
    uint8_t entry_len;
    uint32_t entry_start_ms;

    /* Wrong-entry backoff. RAM only (confirmed decision: resets on
     * reboot — the cheat-code is a low-entropy convenience fallback, not
     * the primary security boundary). */
    uint16_t consecutive_wrong;
    uint32_t backoff_until_ms;
    uint32_t last_attempt_ms;
    bool backoff_active; /* cached by mc_lock_tick(); status reads this directly, no now_ms needed */

    /* Set whenever persisted state ({config, locked_flag}) changed and the
     * platform should call mc_lock_serialize() + save it. Cleared by
     * mc_lock_clear_dirty(). Lock/cheat-code changes are rare and
     * security-relevant, so callers persist immediately rather than
     * debouncing (contrast with mc_persist's config/keystore debounce). */
    bool dirty;
} mc_lock_t;

/* What the platform observes right now, fed into mc_lock_tick(). Ownership
 * of computing these stays with the platform (main.c / sim main.c):
 *   - ignition_switch_level: mc_input_button_level() of
 *     config.ignition_switch_input, or false if unassigned.
 *   - engine_running: mirrors mc_output_engine_t.engine_running
 *     (voltage detection; false if nothing provides it).
 *
 * Phone-as-key is deliberately NOT here — see mc_lock_request_unlock()'s
 * doc comment on why it's edge-triggered (from mc_app_t.on_session_authed)
 * rather than a level polled every tick. */
typedef struct {
    bool ignition_switch_level;
    bool engine_running;
} mc_lock_inputs_t;

typedef enum {
    MC_LOCK_RESULT_OK = 0,
    MC_LOCK_RESULT_REJECTED,       /* guard failed (e.g. lock while ignition live) or nothing to do */
    MC_LOCK_RESULT_UNAUTHORIZED,   /* method not enabled, or immobilizer not configured */
} mc_lock_result_t;

typedef enum {
    MC_LOCK_CHEATCODE_PENDING = 0, /* buffered, not yet a complete-length candidate */
    MC_LOCK_CHEATCODE_MATCH,
    MC_LOCK_CHEATCODE_MISMATCH,
    MC_LOCK_CHEATCODE_IN_BACKOFF,  /* ignored: backoff active */
    MC_LOCK_CHEATCODE_NOT_SET,     /* no cheat-code configured; ignored */
} mc_lock_cheatcode_outcome_t;

/* Initializes `lock` at boot from persisted {config, locked_flag}, applying
 * the ride-safe restore rule (AGENTS.md #1): never restores into LOCKED
 * while the engine appears to be running or the ignition output is
 * already live (a brownout mid-ride must not immobilize a moving bike).
 * If the override fires, `lock->dirty` is set so the platform persists the
 * corrected (unlocked) state. `output` must already have had
 * mc_output_restore_from_config() applied, so its live ignition state is
 * accurate. Applies mc_output_set_immobilized() to match the restored
 * state. Completes in O(1) — safe to call within the <250ms boot budget. */
void mc_lock_init(mc_lock_t *lock, const mc_lock_config_t *config, bool persisted_locked_flag,
                  mc_output_engine_t *output, uint32_t now_ms);

/* Call every ~10ms (same cadence as mc_input_poll). Drives the automatic
 * UNLOCKED <-> PARKED <-> LOCKED transitions (engine/ignition guards, the
 * auto-lock grace timer), passive auto-unlock via the ignition switch, the
 * cheat-code entry timeout, and the backoff quiet-period reset. Also, outside
 * LOCKED, mirrors an active ignition-switch input's level onto the ignition
 * output itself (mc_output_set()), so turning the key off actually drops
 * ignition and lets the parked-guard/auto-lock timer see it. */
void mc_lock_tick(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms,
                  const mc_lock_inputs_t *inputs);

/* Explicit MC_OP_LOCK: locks now if the immobilizer is enabled and the
 * guard (!engine_running && !ignition_live) holds. Idempotent if already
 * LOCKED. Works from UNLOCKED or PARKED (an explicit request doesn't need
 * to wait for the auto-lock grace timer, provided the guard holds). */
mc_lock_result_t mc_lock_request_lock(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms);

/* Phone-as-key unlock (PHONE method, REJECTED if it isn't enabled; a no-op
 * OK if not currently LOCKED). Two callers, both already gated on the
 * session being authenticated:
 *   - The platform's mc_app_t.on_session_authed hook, fired once by
 *     mc_session.c exactly when a session's challenge-response *newly*
 *     succeeds — this is what makes phone-as-key work automatically, on
 *     the rising edge of "a phone just authenticated" (proximity/tap,
 *     AGENTS.md #4). It is deliberately NOT a level checked every tick:
 *     a phone that stays connected (e.g. background BLE) would otherwise
 *     immediately re-unlock the bike on the very next tick after any
 *     explicit MC_OP_LOCK from that same still-connected session,
 *     defeating "lock now" entirely. Edge-triggering means locking while
 *     a phone is already connected correctly stays locked, matching how
 *     a rider would expect "lock now" to behave even standing next to
 *     the bike.
 *   - The explicit MC_OP_UNLOCK command, for a session that was already
 *     authenticated *before* the bike locked (so the edge already fired
 *     and won't fire again without a reconnect) and the rider explicitly
 *     asks to unlock. */
mc_lock_result_t mc_lock_request_unlock(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms);

/* Feed one MC_PRESS_SHORT event's button index (only short presses count,
 * matching mc_input's own sequence-combo matcher). Buffers into the
 * in-progress entry; evaluates against the stored hash as soon as the
 * buffer reaches the configured length. */
mc_lock_cheatcode_outcome_t mc_lock_cheatcode_press(mc_lock_t *lock, mc_output_engine_t *output,
                                                    uint32_t now_ms, uint8_t button);

/* MC_OP_CHEATCODE_SET: hashes+salts and stores a new cheat-code, replacing
 * any existing one. Resets the in-progress entry buffer and the backoff.
 * Returns false (BAD_REQUEST) if len is out of [MIN,MAX] or any button
 * index is out of range. Does not itself change immobilizer_enabled. */
bool mc_lock_set_cheatcode(mc_lock_t *lock, const uint8_t *buttons, uint8_t len, uint32_t now_ms);

/* MC_OP_CHEATCODE_CLEAR: refuses (returns false / REJECTED) while
 * config.immobilizer_enabled is true — the cheat-code is the mandatory
 * fallback and can't be removed out from under an active immobilizer
 * (AGENTS.md #3). Disable the immobilizer first. */
bool mc_lock_clear_cheatcode(mc_lock_t *lock, uint32_t now_ms);

/* MC_OP_CHEATCODE_TEST: pure comparison against the stored hash. Never
 * touches entry buffer, backoff, or lock state — practice mode. */
bool mc_lock_test_cheatcode(const mc_lock_config_t *cfg, const uint8_t *buttons, uint8_t len);

/* MC_OP_LOCK_SET_CONFIG: validates (mc_lock_config_validate) against
 * `outputs`, and on success applies the non-cheatcode fields (leaving
 * cheatcode_set/len/salt/hash untouched — the cheat-code is only ever
 * changed via mc_lock_set_cheatcode/mc_lock_clear_cheatcode). Handles the
 * DISABLED<->UNLOCKED edge transitions and releasing the immobilize flag
 * if disabling while LOCKED. Returns the validate bitmask (0 = applied). */
uint32_t mc_lock_apply_config(mc_lock_t *lock, mc_output_engine_t *output, const mc_output_config_t *outputs,
                              bool immobilizer_enabled, uint8_t methods_mask, int8_t ignition_switch_input,
                              uint32_t auto_lock_grace_ms, uint32_t cheatcode_window_ms, uint32_t now_ms);

/* MC_OP_TRANSFER_OWNERSHIP's lock-side effect: disables the immobilizer,
 * releases any ignition inhibit, clears the cheat-code and all lock
 * config back to defaults, resets backoff. Does NOT touch the keystore —
 * the session handler wipes that separately. */
void mc_lock_transfer_ownership(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms);

/* Physical factory reset's lock-side effect: identical end state to
 * mc_lock_transfer_ownership (both must leave the immobilizer disabled and
 * un-strand the next owner), exposed under its own name so call sites read
 * clearly. */
static inline void mc_lock_factory_reset(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms)
{
    mc_lock_transfer_ownership(lock, output, now_ms);
}

mc_lock_state_t mc_lock_wire_state(const mc_lock_t *lock);
static inline bool mc_lock_backoff_active(const mc_lock_t *lock)
{
    return lock->backoff_active;
}

/* The platform logs MC_EVT_CHEATCODE_LOCKOUT (mc_event_log.h)
 * exactly once per lockout, right after a mc_lock_cheatcode_press() call
 * that returns MC_LOCK_CHEATCODE_MISMATCH and leaves backoff_active newly
 * true (backoff_active can only transition false->true inside that same
 * call — a press while already in backoff short-circuits to
 * MC_LOCK_CHEATCODE_IN_BACKOFF before ever reaching the mismatch path). This
 * accessor supplies the event's arg0. */
static inline uint16_t mc_lock_wrong_attempt_count(const mc_lock_t *lock)
{
    return lock->consecutive_wrong;
}

static inline bool mc_lock_is_dirty(const mc_lock_t *lock)
{
    return lock->dirty;
}
static inline void mc_lock_clear_dirty(mc_lock_t *lock)
{
    lock->dirty = false;
}

/* --- persistence: own NVS blob, versioned envelope like mc_keystore --- */

typedef enum {
    MC_LOCK_STORE_OK = 0,
    MC_LOCK_STORE_ERR_BUFFER_TOO_SMALL,
    MC_LOCK_STORE_ERR_CORRUPT,
    MC_LOCK_STORE_ERR_FUTURE_VERSION,
} mc_lock_store_result_t;

/* Serializes {config, locked_flag} — nothing else (entry buffer, backoff,
 * and `state` itself are RAM-only / re-derived by mc_lock_init). */
mc_lock_store_result_t mc_lock_serialize(const mc_lock_t *lock, uint8_t *buf, size_t buf_len, size_t *out_len);
mc_lock_store_result_t mc_lock_deserialize(const uint8_t *buf, size_t len, mc_lock_config_t *out_config,
                                           bool *out_locked_flag);
