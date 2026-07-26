#pragma once

/*
 * mc_session — the transport-agnostic protocol + auth engine.
 *
 * One mc_session_t exists per client connection (per BLE link, per
 * WebSocket connection). It owns that connection's authentication state:
 * unauthenticated clients may only read status and run the
 * challenge-response / first-key enrollment; everything that changes state
 * (control, config write) requires an authenticated session (AGENTS.md:
 * "All state-changing writes require the session to be authenticated via
 * the challenge-response").
 *
 * Shared device state (output engine, config, keystore) lives in a single
 * mc_app_t that every session points at. Transports call mc_session_handle()
 * with an inbound (channel, payload) and a send callback; the session emits
 * zero or more response frames through that callback. No I/O, no clock, no
 * globals — fully unit-testable on the host.
 */

#include "mc_config_json.h"
#include "mc_diag.h"
#include "mc_event_log.h"
#include "mc_keystore.h"
#include "mc_lock.h"
#include "mc_ota.h"
#include "mc_output.h"
#include "mc_protocol.h"
#include "mc_status.h"

/* Send one response frame on `ch`. Provided by the transport: NimBLE
 * notifies the matching characteristic; the WS server sends a binary
 * message [ch][payload]; tests record it. */
typedef void (*mc_send_fn)(void *io, mc_channel_t ch, const uint8_t *data, size_t len);

/* Shared device state + platform hooks, pointed at by every session. */
typedef struct {
    mc_output_engine_t *output;
    mc_config_t *config;
    mc_keystore_t *keystore;
    mc_lock_t *lock; /* may be NULL (then lock ops report REJECTED and status reports UNKNOWN) */
    mc_diag_t *diag; /* may be NULL (then diag ops report REJECTED and status battery/fault stay 0) */
    mc_ota_t *ota;   /* may be NULL (then every OTA op reports REJECTED) */
    mc_event_log_t *event_log; /* may be NULL (then EVENT_LOG_GET reports REJECTED) */

    /* Platform fills the fields it owns (uptime_ms, battery_mv, rssi_dbm);
     * the session overlays output_state_mask from `output`, lock_state +
     * the backoff bit from `lock`, and the firmware version. May be NULL
     * (then only version + outputs + lock are reported). */
    void (*fill_status)(mc_status_t *out, void *app_ctx);

    /* Called after the session commits a config change / keystore change,
     * so the platform can schedule a debounced persist (mc_persist + NVS).
     * May be NULL on hosts that don't persist. */
    void (*persist_config)(void *app_ctx);
    void (*persist_keystore)(void *app_ctx);

    /* Called after any mutating lock op (lock/unlock, cheat-code set/clear,
     * config apply, ownership transfer) — lock state persists immediately
     * rather than through mc_persist's debounce (mc_lock.h: rare and
     * security-relevant enough to accept the extra flash write). May be
     * NULL on hosts that don't persist. */
    void (*persist_lock)(void *app_ctx);

    /* Called after MC_OP_DIAG_SET_CALIB applies new board calibration
     * constants (mc_diag_t.calib) — persists immediately, like persist_lock,
     * rather than through mc_persist's debounce: a deliberate, rare,
     * installer/bench action, not a hot path. Diagnostics THRESHOLD config
     * (mc_diag_t.config) is not a separate hook — it rides mc_config_t's
     * existing persist_config path, since it lives inside mc_config_t (see
     * mc_diag.h). May be NULL on hosts that don't persist. */
    void (*persist_diag_calib)(void *app_ctx);

    /* Called exactly once, synchronously, whenever a session's
     * challenge-response *newly* succeeds (the AUTH_RESPONSE handler's
     * success path) — never on an already-authenticated session doing
     * something else. The platform wires this to
     * mc_lock_request_unlock(lock, output, now_ms) (mc_lock.h) so
     * phone-as-key auto-unlock is edge-triggered on "a phone just
     * authenticated" rather than a level polled every tick — see
     * mc_lock_request_unlock()'s doc comment for why that distinction
     * matters (a level would make an explicit MC_OP_LOCK self-defeating
     * while that same phone stays connected). May be NULL (then
     * phone-as-key never auto-unlocks — safe default, just inert; an
     * explicit MC_OP_UNLOCK still works). */
    void (*on_session_authed)(void *app_ctx);

    /* Appends one mc_event_log record (mc_event_log.h) with the
     * platform's own clock, same doctrine as the persist_* hooks above (no
     * now_ms parameter — the session has no clock by design, see
     * mc_types.h). May be NULL (then nothing is ever logged, e.g. hosts
     * without an event_log attached; harmless — logging failures are never
     * fatal to the operation they're logging). */
    void (*log_event)(void *app_ctx, uint8_t type, uint8_t arg0, uint8_t arg1);

    void *app_ctx;
} mc_app_t;

typedef enum {
    MC_SESSION_UNAUTH = 0,
    MC_SESSION_CHALLENGED, /* a challenge nonce has been issued */
    MC_SESSION_AUTHED,
} mc_session_state_t;

typedef struct {
    mc_session_state_t state;
    uint8_t nonce[MC_CRYPTO_NONCE_BYTES];
    int authed_slot; /* keystore slot that authenticated, or -1 */

    /* CONFIG_WRITE staging. */
    uint8_t cfg_write_buf[MC_CONFIG_JSON_MAX];
    uint16_t cfg_write_total;
    uint16_t cfg_write_got;
    bool cfg_write_active;
} mc_session_t;

/* Resets a session to unauthenticated. Call when a new connection opens. */
void mc_session_init(mc_session_t *s);

/* True once the client has completed challenge-response on this session. */
static inline bool mc_session_is_authed(const mc_session_t *s)
{
    return s->state == MC_SESSION_AUTHED;
}

/* Handles one inbound frame (channel + opcode-led payload), emitting any
 * response frames via `send`. Safe against truncated/malformed input. */
void mc_session_handle(mc_session_t *s, mc_app_t *app,
                       mc_channel_t ch, const uint8_t *payload, size_t len,
                       mc_send_fn send, void *io);

/* Builds the exact message a client signs for the challenge-response:
 * MC_AUTH_CONTEXT bytes followed by the 32-byte nonce. Writes to `out`
 * (must be >= MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES) and returns the
 * length. Exposed so tests (and the documented reference) build it the
 * same way the device does. */
size_t mc_session_build_auth_message(const uint8_t nonce[MC_CRYPTO_NONCE_BYTES], uint8_t *out);
