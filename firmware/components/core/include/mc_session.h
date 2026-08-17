#pragma once

/*
 * mc_session — the transport-agnostic protocol + auth engine.
 *
 * One mc_session_t exists per client connection (per BLE link, per
 * WebSocket connection). It owns that connection's authentication state:
 * unauthenticated clients may only read status and run the
 * challenge-response / first-key enrollment; everything that changes state
 * (control, config write) requires an authenticated session.
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
    /* The live input engine, so a CONFIG_WRITE commit can push new chord
     * definitions and press timing into it (mc_input_set_config). May be NULL
     * — then input config changes only take effect on the next boot, which is
     * the bug this pointer exists to prevent. */
    mc_input_engine_t *input;

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

    /* Called when config->device_name has just changed — a config write that
     * renamed the board, or an ownership transfer resetting it. The platform
     * wires this to whatever republishes the name (on ESP32: the GAP device
     * name plus a re-advertise, since the name lives in the advertising
     * payload and a running advertisement keeps the old one until it is
     * rebuilt). May be NULL on hosts with no radio — the sim has none, and
     * the name is still stored and exported normally there. */
    void (*on_device_name_changed)(void *app_ctx);

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

    /* MC_OP_INPUT_LEARN: push input events to this session (mc_protocol.h).
     * Cleared by mc_session_init(), so it can never outlive the BLE link. */
    bool input_learn;
    /* INPUT_LEARN's optional second byte: while set, the platform must not
     * run handlebar bindings for the presses it is reporting.
     *
     * For entering a cheat-code, where the rider is pressing whichever
     * buttons make up their code and would otherwise sound the horn or
     * flash the indicators four to ten times. Handlebar controls are inert
     * for the duration, which is an acceptable trade only because the
     * rider deliberately entered that mode and is standing at the bike.
     *
     * Deliberately does NOT gate the cheat-code matcher itself
     * (mc_lock_cheatcode_press) or the brake-switch pass-through: the first
     * is layered unlock's unlock fallback and must never be disableable from
     * the app, the second is brake-light priority's brake-light guarantee. */
    bool input_learn_suppress_actions;
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
