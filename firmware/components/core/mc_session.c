#include "mc_session.h"

#include <string.h>

#include "mc_crypto.h"

void mc_session_init(mc_session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->state = MC_SESSION_UNAUTH;
    s->authed_slot = -1;
}

size_t mc_session_build_auth_message(const uint8_t nonce[MC_CRYPTO_NONCE_BYTES], uint8_t *out)
{
    memcpy(out, MC_AUTH_CONTEXT, MC_AUTH_CONTEXT_LEN);
    memcpy(out + MC_AUTH_CONTEXT_LEN, nonce, MC_CRYPTO_NONCE_BYTES);
    return MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES;
}

/* --- frame send helpers --- */

#define MC_FRAME_MAX 256

static void send_frame(mc_send_fn send, void *io, mc_channel_t ch,
                       uint8_t opcode, const uint8_t *payload, size_t plen)
{
    uint8_t buf[MC_FRAME_MAX];
    if (plen > sizeof(buf) - 1) {
        return; /* internal guard: responses are bounded well under this */
    }
    buf[0] = opcode;
    if (plen > 0) {
        memcpy(buf + 1, payload, plen);
    }
    send(io, ch, buf, plen + 1);
}

static void send_result2(mc_send_fn send, void *io, mc_channel_t ch,
                         uint8_t opcode, uint8_t a, uint8_t b)
{
    uint8_t p[2] = { a, b };
    send_frame(send, io, ch, opcode, p, 2);
}

static void send_result1(mc_send_fn send, void *io, mc_channel_t ch,
                         uint8_t opcode, uint8_t a)
{
    send_frame(send, io, ch, opcode, &a, 1);
}

/* --- STATUS --- */

static void build_status(mc_session_t *s, mc_app_t *app, mc_status_t *st)
{
    (void)s;
    mc_status_init(st);
    if (app->fill_status != NULL) {
        app->fill_status(st, app->app_ctx);
    }
    uint16_t mask = 0;
    if (app->output != NULL) {
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            if (mc_output_get_state(app->output, ch)) {
                mask |= (uint16_t)(1u << ch);
            }
        }
    }
    st->output_state_mask = mask;
    if (app->output != NULL) {
        st->lv_cutoff_active = mc_output_lv_cutoff_active(app->output);
    }
    if (app->lock != NULL) {
        st->lock_state = mc_lock_wire_state(app->lock);
        st->cheatcode_backoff = mc_lock_backoff_active(app->lock);
    }
    if (app->diag != NULL) {
        st->battery_mv = mc_diag_get_battery_mv(app->diag);
        st->output_fault_mask = mc_diag_get_fault_mask(app->diag);
    }
}

static void handle_status(mc_session_t *s, mc_app_t *app,
                          const uint8_t *payload, size_t len, mc_send_fn send, void *io)
{
    if (len < 1) {
        return;
    }
    if (payload[0] == MC_OP_STATUS_GET) {
        mc_status_t st;
        build_status(s, app, &st);
        uint8_t wire[MC_STATUS_WIRE_LEN];
        mc_status_serialize(&st, wire, sizeof(wire));
        send_frame(send, io, MC_CH_STATUS, MC_OP_STATUS, wire, sizeof(wire));
    }
}

/* --- AUTH --- */

static void handle_auth(mc_session_t *s, mc_app_t *app,
                        const uint8_t *payload, size_t len, mc_send_fn send, void *io)
{
    if (len < 1) {
        return;
    }
    uint8_t op = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = len - 1;

    switch (op) {
    case MC_OP_AUTH_BEGIN: {
        if (!mc_crypto_random(s->nonce, MC_CRYPTO_NONCE_BYTES)) {
            /* CSPRNG failure: refuse to issue a weak challenge. */
            send_result2(send, io, MC_CH_AUTH, MC_OP_AUTH_RESULT, MC_RESULT_INTERNAL, 0xFF);
            return;
        }
        s->state = MC_SESSION_CHALLENGED;
        send_frame(send, io, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE, s->nonce, MC_CRYPTO_NONCE_BYTES);
        break;
    }
    case MC_OP_AUTH_RESPONSE: {
        if (s->state != MC_SESSION_CHALLENGED || body_len != MC_CRYPTO_SIG_BYTES) {
            send_result2(send, io, MC_CH_AUTH, MC_OP_AUTH_RESULT, MC_RESULT_BAD_REQUEST, 0xFF);
            return;
        }
        uint8_t msg[MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES];
        size_t msg_len = mc_session_build_auth_message(s->nonce, msg);
        int slot = (app->keystore != NULL)
                       ? mc_keystore_find_verifying(app->keystore, msg, msg_len, body)
                       : -1;
        /* Single-use nonce: invalidate it whatever the outcome, so a
         * captured signature can never be replayed on this session. */
        memset(s->nonce, 0, sizeof(s->nonce));
        if (slot >= 0) {
            s->state = MC_SESSION_AUTHED;
            s->authed_slot = slot;
            send_result2(send, io, MC_CH_AUTH, MC_OP_AUTH_RESULT, MC_RESULT_OK, (uint8_t)slot);
            if (app->on_session_authed != NULL) {
                app->on_session_authed(app->app_ctx);
            }
        } else {
            s->state = MC_SESSION_UNAUTH;
            s->authed_slot = -1;
            send_result2(send, io, MC_CH_AUTH, MC_OP_AUTH_RESULT, MC_RESULT_REJECTED, 0xFF);
        }
        break;
    }
    case MC_OP_ENROLL: {
        if (body_len < MC_CRYPTO_PUBKEY_BYTES) {
            send_result2(send, io, MC_CH_AUTH, MC_OP_ENROLL_RESULT, MC_RESULT_BAD_REQUEST, 0xFF);
            return;
        }
        /* Enrollment is permitted only when the keystore is empty
         * (trust-on-first-use for the first key on a fresh/factory-reset
         * device) or when an already-authenticated owner authorizes an
         * additional key. (An earlier forward-reference here anticipated
         * the immobilizer work adding physical-button confirmation on top
         * of this rule for the unattended-bike case; that turned out to be
         * outside its approved scope — see docs/PROTOCOL.md §6.) */
        bool permitted = (app->keystore != NULL) &&
                         (mc_keystore_is_empty(app->keystore) || s->state == MC_SESSION_AUTHED);
        if (!permitted) {
            send_result2(send, io, MC_CH_AUTH, MC_OP_ENROLL_RESULT, MC_RESULT_ENROLL_DENIED, 0xFF);
            return;
        }
        char label[MC_KEY_LABEL_MAX];
        size_t label_len = body_len - MC_CRYPTO_PUBKEY_BYTES;
        if (label_len > MC_KEY_LABEL_MAX - 1) {
            label_len = MC_KEY_LABEL_MAX - 1;
        }
        memcpy(label, body + MC_CRYPTO_PUBKEY_BYTES, label_len);
        label[label_len] = '\0';

        int slot = mc_keystore_add(app->keystore, body, label);
        if (slot < 0) {
            send_result2(send, io, MC_CH_AUTH, MC_OP_ENROLL_RESULT, MC_RESULT_KEYSTORE_FULL, 0xFF);
            return;
        }
        if (app->persist_keystore != NULL) {
            app->persist_keystore(app->app_ctx);
        }
        if (app->log_event != NULL) {
            app->log_event(app->app_ctx, MC_EVT_KEY_ENROLLED, (uint8_t)slot, 0);
        }
        send_result2(send, io, MC_CH_AUTH, MC_OP_ENROLL_RESULT, MC_RESULT_OK, (uint8_t)slot);
        break;
    }
    case MC_OP_KEY_LIST: {
        if (s->state != MC_SESSION_AUTHED) {
            send_result1(send, io, MC_CH_AUTH, MC_OP_KEY_LIST_RESULT, 0); /* count 0 for unauth */
            return;
        }
        uint8_t buf[1 + MC_KEYSTORE_MAX_KEYS * (2 + MC_KEY_LABEL_MAX)];
        size_t pos = 1;
        uint8_t count = 0;
        for (int i = 0; i < MC_KEYSTORE_MAX_KEYS; i++) {
            char label[MC_KEY_LABEL_MAX];
            if (!mc_keystore_get(app->keystore, i, NULL, label, sizeof(label))) {
                continue;
            }
            uint8_t llen = (uint8_t)strlen(label);
            buf[pos++] = (uint8_t)i;
            buf[pos++] = llen;
            memcpy(buf + pos, label, llen);
            pos += llen;
            count++;
        }
        buf[0] = count;
        send_frame(send, io, MC_CH_AUTH, MC_OP_KEY_LIST_RESULT, buf, pos);
        break;
    }
    case MC_OP_KEY_REVOKE: {
        if (s->state != MC_SESSION_AUTHED || body_len < 1) {
            send_result2(send, io, MC_CH_AUTH, MC_OP_KEY_REVOKE_RESULT,
                         s->state == MC_SESSION_AUTHED ? MC_RESULT_BAD_REQUEST : MC_RESULT_UNAUTHENTICATED, 0xFF);
            return;
        }
        uint8_t slot = body[0];
        bool removed = mc_keystore_remove(app->keystore, slot);
        if (removed) {
            if (app->persist_keystore != NULL) {
                app->persist_keystore(app->app_ctx);
            }
            if (app->log_event != NULL) {
                app->log_event(app->app_ctx, MC_EVT_KEY_REVOKED, slot, 0);
            }
        }
        send_result2(send, io, MC_CH_AUTH, MC_OP_KEY_REVOKE_RESULT,
                     removed ? MC_RESULT_OK : MC_RESULT_NOT_FOUND, slot);
        break;
    }
    default:
        break;
    }
}

/* --- COMMAND --- */

static void handle_command(mc_session_t *s, mc_app_t *app,
                           const uint8_t *payload, size_t len, mc_send_fn send, void *io)
{
    if (len < 1) {
        return;
    }
    uint8_t op = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = len - 1;

    if (s->state != MC_SESSION_AUTHED) {
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_UNAUTHENTICATED);
        return;
    }

    switch (op) {
    case MC_OP_SET_OUTPUT: {
        if (body_len < 2) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint8_t channel = body[0];
        bool on = body[1] != 0;
        mc_output_result_t r = mc_output_set(app->output, channel, on, MC_OUT_SRC_REMOTE);
        uint8_t result;
        switch (r) {
        case MC_OUT_OK:
            result = MC_RESULT_OK;
            if (app->persist_config != NULL) {
                app->persist_config(app->app_ctx);
            }
            break;
        case MC_OUT_ERR_BAD_CHANNEL:
            result = MC_RESULT_BAD_REQUEST;
            break;
        default:
            /* starter remote-blocked / engine-running / interlock, etc.
             * (AGENTS.md #6). The engine is the authority; BLE never
             * overrides it. */
            result = MC_RESULT_REJECTED;
            break;
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, result);
        break;
    }

    /* Hazard toggle. Plain turn-signal control stays on
     * MC_OP_SET_OUTPUT above — mutual exclusion/auto-cancel are embedded in
     * mc_output_set() itself — but hazard genuinely needs its own entry
     * point (see mc_output.h), so it gets its own opcode. */
    case MC_OP_HAZARD_PRESS: {
        int l = mc_output_find_channel_by_function(&app->output->config, MC_OUT_FUNC_TURN_L);
        int r = mc_output_find_channel_by_function(&app->output->config, MC_OUT_FUNC_TURN_R);
        if (l < 0 && r < 0) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        mc_output_hazard_press(app->output, app->output->last_tick_ms);
        if (app->persist_config != NULL) {
            app->persist_config(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_OK);
        break;
    }

    /* --- Lock / immobilizer ops. All share this channel's
     * existing authed gate above. `now_ms` doesn't reach mc_session (it
     * has no clock, by design — see mc_types.h); every mc_lock entry point
     * called from here is a one-shot command whose behavior doesn't
     * depend on the timestamp (only mc_lock_tick()/mc_lock_cheatcode_press(),
     * ticked from the app task with a real clock, use it), so 0 is
     * passed and is provably inert. */
    case MC_OP_LOCK: {
        if (app->lock == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        mc_lock_result_t r = mc_lock_request_lock(app->lock, app->output, 0);
        /* Gate on mc_lock_is_dirty(), not just result==OK — an idempotent
         * "already LOCKED" OK shouldn't cost a flash write (or a log entry:
         * same reasoning applies to MC_EVT_LOCK_ENGAGED below). */
        if (mc_lock_is_dirty(app->lock)) {
            if (app->persist_lock != NULL) {
                app->persist_lock(app->app_ctx);
            }
            if (app->log_event != NULL) {
                app->log_event(app->app_ctx, MC_EVT_LOCK_ENGAGED, 0, 0);
            }
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op,
                     r == MC_LOCK_RESULT_OK ? MC_RESULT_OK : MC_RESULT_REJECTED);
        break;
    }
    case MC_OP_UNLOCK: {
        if (app->lock == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        mc_lock_result_t r = mc_lock_request_unlock(app->lock, app->output, 0);
        if (mc_lock_is_dirty(app->lock)) {
            if (app->persist_lock != NULL) {
                app->persist_lock(app->app_ctx);
            }
            if (app->log_event != NULL) {
                app->log_event(app->app_ctx, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_EXPLICIT, 0);
            }
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op,
                     r == MC_LOCK_RESULT_OK ? MC_RESULT_OK : MC_RESULT_REJECTED);
        break;
    }
    case MC_OP_LOCK_GET_CONFIG: {
        if (app->lock == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        const mc_lock_config_t *cfg = &app->lock->config;
        uint8_t resp[9];
        resp[0] = cfg->immobilizer_enabled ? 1 : 0;
        resp[1] = cfg->methods_mask;
        resp[2] = (cfg->ignition_switch_input < 0) ? 0xFF : (uint8_t)cfg->ignition_switch_input;
        uint16_t grace = cfg->auto_lock_grace_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)cfg->auto_lock_grace_ms;
        uint16_t window = cfg->cheatcode_window_ms > 0xFFFFu ? 0xFFFFu : (uint16_t)cfg->cheatcode_window_ms;
        mc_put_u16le(&resp[3], grace);
        mc_put_u16le(&resp[5], window);
        resp[7] = cfg->cheatcode_set ? 1 : 0;
        resp[8] = cfg->cheatcode_len;
        send_frame(send, io, MC_CH_COMMAND, MC_OP_LOCK_CONFIG, resp, sizeof(resp));
        break;
    }
    case MC_OP_LOCK_SET_CONFIG: {
        if (app->lock == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        if (body_len < 7) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        bool enabled = body[0] != 0;
        uint8_t methods_mask = body[1];
        int8_t ign_input = (body[2] == 0xFF) ? -1 : (int8_t)body[2];
        uint32_t grace_ms = mc_get_u16le(&body[3]);
        uint32_t window_ms = mc_get_u16le(&body[5]);
        uint32_t flags = mc_lock_apply_config(app->lock, app->output, &app->output->config,
                                              enabled, methods_mask, ign_input, grace_ms, window_ms, 0);
        if (flags != MC_LOCK_CFG_OK) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        if (app->persist_lock != NULL) {
            app->persist_lock(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_OK);
        break;
    }
    case MC_OP_CHEATCODE_SET: {
        if (app->lock == NULL || body_len < 1 || body_len != (size_t)(1 + body[0])) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint8_t clen = body[0];
        bool ok = mc_lock_set_cheatcode(app->lock, body + 1, clen, 0);
        if (ok && app->persist_lock != NULL) {
            app->persist_lock(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, ok ? MC_RESULT_OK : MC_RESULT_BAD_REQUEST);
        break;
    }
    case MC_OP_CHEATCODE_CLEAR: {
        if (app->lock == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        bool ok = mc_lock_clear_cheatcode(app->lock, 0);
        if (ok && app->persist_lock != NULL) {
            app->persist_lock(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, ok ? MC_RESULT_OK : MC_RESULT_REJECTED);
        break;
    }
    case MC_OP_CHEATCODE_TEST: {
        /* Practice mode: pure comparison, no side effects (never touches
         * the entry buffer, backoff, or lock state). Authed-only (this
         * whole handler is), same trust boundary as every other command —
         * see mc_lock.h's header comment on why that's an acceptable
         * bound rather than a new brute-force surface. */
        if (app->lock == NULL || body_len < 1 || body_len != (size_t)(1 + body[0])) {
            uint8_t resp[2] = { MC_RESULT_BAD_REQUEST, 0 };
            send_frame(send, io, MC_CH_COMMAND, MC_OP_CHEATCODE_TEST_RESULT, resp, sizeof(resp));
            return;
        }
        uint8_t clen = body[0];
        bool match = mc_lock_test_cheatcode(&app->lock->config, body + 1, clen);
        uint8_t resp[2] = { MC_RESULT_OK, match ? 1 : 0 };
        send_frame(send, io, MC_CH_COMMAND, MC_OP_CHEATCODE_TEST_RESULT, resp, sizeof(resp));
        break;
    }
    case MC_OP_TRANSFER_OWNERSHIP: {
        if (app->keystore != NULL) {
            mc_keystore_wipe(app->keystore);
        }
        if (app->lock != NULL) {
            mc_lock_transfer_ownership(app->lock, app->output, 0);
            if (app->persist_lock != NULL) {
                app->persist_lock(app->app_ctx);
            }
        }
        if (app->persist_keystore != NULL) {
            app->persist_keystore(app->app_ctx);
        }
        /* Wipe the event log's own history too, then log the wipe itself as
         * the sole surviving record (mc_event_log_clear() + one append) —
         * consistent with this op already being a full wipe of
         * keystore/lock/config; see docs/PROTOCOL.md §15. */
        if (app->event_log != NULL) {
            mc_event_log_clear(app->event_log);
        }
        if (app->log_event != NULL) {
            app->log_event(app->app_ctx, MC_EVT_OWNERSHIP_TRANSFERRED, 0, 0);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_OK);
        break;
    }

    /* --- Diagnostics ops (docs/PROTOCOL.md §12). Same authed gate
     * above. Every dedicated response leads with [result:1] so a device
     * with no mc_diag attached (app->diag == NULL) can still reply with a
     * well-formed, zeroed frame carrying MC_RESULT_REJECTED, rather than
     * needing a second error path — see mc_protocol.h's doc comment. */
    case MC_OP_DIAG_GET: {
        uint8_t resp[1 + MC_OUTPUT_COUNT * 2 + MC_OUTPUT_COUNT];
        memset(resp, 0, sizeof(resp));
        if (app->diag == NULL) {
            resp[0] = MC_RESULT_REJECTED;
            send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_RESULT, resp, sizeof(resp));
            return;
        }
        resp[0] = MC_RESULT_OK;
        size_t pos = 1;
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            mc_put_u16le(&resp[pos], app->diag->current_ma[ch]);
            pos += 2;
        }
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            resp[pos++] = (uint8_t)app->diag->fault[ch];
        }
        send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_RESULT, resp, sizeof(resp));
        break;
    }
    case MC_OP_DIAG_GET_CONFIG: {
        uint8_t resp[1 + MC_OUTPUT_COUNT * 4 + 8];
        memset(resp, 0, sizeof(resp));
        if (app->diag == NULL) {
            resp[0] = MC_RESULT_REJECTED;
            send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_CONFIG, resp, sizeof(resp));
            return;
        }
        resp[0] = MC_RESULT_OK;
        size_t pos = 1;
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            mc_put_u16le(&resp[pos], app->diag->config.channels[ch].open_load_ma);
            pos += 2;
            mc_put_u16le(&resp[pos], app->diag->config.channels[ch].overcurrent_ma);
            pos += 2;
        }
        mc_put_u16le(&resp[pos], app->diag->config.lv_cutoff_mv); pos += 2;
        mc_put_u16le(&resp[pos], app->diag->config.lv_cutoff_hysteresis_mv); pos += 2;
        mc_put_u16le(&resp[pos], app->diag->config.engine_run_mv); pos += 2;
        mc_put_u16le(&resp[pos], app->diag->config.engine_run_hysteresis_mv); pos += 2;
        send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_CONFIG, resp, sizeof(resp));
        break;
    }
    case MC_OP_DIAG_SET_CONFIG: {
        if (app->diag == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        size_t needed = (size_t)MC_OUTPUT_COUNT * 4 + 8;
        if (body_len < needed) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        mc_diag_config_t cfg;
        size_t pos = 0;
        for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
            cfg.channels[ch].open_load_ma = mc_get_u16le(&body[pos]); pos += 2;
            cfg.channels[ch].overcurrent_ma = mc_get_u16le(&body[pos]); pos += 2;
        }
        cfg.lv_cutoff_mv = mc_get_u16le(&body[pos]); pos += 2;
        cfg.lv_cutoff_hysteresis_mv = mc_get_u16le(&body[pos]); pos += 2;
        cfg.engine_run_mv = mc_get_u16le(&body[pos]); pos += 2;
        cfg.engine_run_hysteresis_mv = mc_get_u16le(&body[pos]); pos += 2;

        if (mc_diag_config_validate(&cfg) != MC_DIAG_CFG_OK) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        mc_diag_apply_config(app->diag, &cfg);
        if (app->config != NULL) {
            /* Diagnostics thresholds ride mc_config_t (the exportable JSON
             * config) — keep the persisted copy in sync with the live
             * engine's, same double-bookkeeping config_commit() below does
             * for outputs. */
            app->config->diagnostics = cfg;
        }
        if (app->persist_config != NULL) {
            app->persist_config(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_OK);
        break;
    }
    case MC_OP_DIAG_GET_CALIB: {
        uint8_t resp[1 + 16];
        memset(resp, 0, sizeof(resp));
        if (app->diag == NULL) {
            resp[0] = MC_RESULT_REJECTED;
            send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_CALIB, resp, sizeof(resp));
            return;
        }
        resp[0] = MC_RESULT_OK;
        mc_put_f32le(&resp[1], app->diag->calib.is_gain);
        mc_put_i16le(&resp[5], app->diag->calib.is_offset_mv);
        mc_put_f32le(&resp[7], app->diag->calib.kilis);
        mc_put_f32le(&resp[11], app->diag->calib.vbat_gain);
        mc_put_i16le(&resp[15], app->diag->calib.vbat_offset_mv);
        send_frame(send, io, MC_CH_COMMAND, MC_OP_DIAG_CALIB, resp, sizeof(resp));
        break;
    }
    case MC_OP_DIAG_SET_CALIB: {
        if (app->diag == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        if (body_len < 16) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        mc_diag_calib_t calib;
        calib.is_gain = mc_get_f32le(&body[0]);
        calib.is_offset_mv = mc_get_i16le(&body[4]);
        calib.kilis = mc_get_f32le(&body[6]);
        calib.vbat_gain = mc_get_f32le(&body[10]);
        calib.vbat_offset_mv = mc_get_i16le(&body[14]);
        mc_diag_apply_calib(app->diag, &calib);
        if (app->persist_diag_calib != NULL) {
            app->persist_diag_calib(app->app_ctx);
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_OK);
        break;
    }
    case MC_OP_DIAG_LEARN: {
        if (app->diag == NULL) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_REJECTED);
            return;
        }
        if (body_len < 1) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        bool ok = mc_diag_learn(app->diag, app->output, body[0]);
        if (ok) {
            if (app->config != NULL) {
                app->config->diagnostics = app->diag->config;
            }
            if (app->persist_config != NULL) {
                app->persist_config(app->app_ctx);
            }
        }
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, ok ? MC_RESULT_OK : MC_RESULT_REJECTED);
        break;
    }

    /* --- Event log (docs/PROTOCOL.md §15). Same authed gate
     * above. Mirrors CONFIG_CHUNK's [offset/index:u16][total:u16] chunked
     * reassembly idiom (config_send_read() below), keyed by record index
     * instead of byte offset, capped at MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS
     * per frame to keep each frame's stack buffer small (a device could
     * have up to MC_EVENT_LOG_SLOT_COUNT=1024 records; nowhere near that
     * many bytes ever sit on the stack at once). */
    case MC_OP_EVENT_LOG_GET: {
        if (app->event_log == NULL) {
            uint8_t hdr[5] = {0}; /* index=0,total=0,count=0 */
            send_frame(send, io, MC_CH_COMMAND, MC_OP_EVENT_LOG_CHUNK, hdr, sizeof(hdr));
            return;
        }
        if (body_len < 4) {
            send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint32_t since_seq = mc_get_u32le(body);
        size_t total = mc_event_log_count_since(app->event_log, since_seq);
        uint16_t total16 = (total > 0xFFFFu) ? 0xFFFFu : (uint16_t)total;

        uint16_t sent = 0;
        do {
            mc_event_record_t batch[MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS];
            size_t got = mc_event_log_read(app->event_log, since_seq, batch, MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS);

            uint8_t frame[5 + MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS * MC_EVENT_LOG_RECORD_BYTES];
            mc_put_u16le(frame, sent);
            mc_put_u16le(frame + 2, total16);
            frame[4] = (uint8_t)got;
            size_t pos = 5;
            for (size_t i = 0; i < got; i++) {
                mc_put_u32le(&frame[pos], batch[i].seq); pos += 4;
                mc_put_u32le(&frame[pos], batch[i].uptime_ms); pos += 4;
                frame[pos++] = batch[i].type;
                frame[pos++] = batch[i].arg0;
                frame[pos++] = batch[i].arg1;
                frame[pos++] = batch[i].reserved;
                since_seq = batch[i].seq; /* advance the cursor for the next batch */
            }
            send_frame(send, io, MC_CH_COMMAND, MC_OP_EVENT_LOG_CHUNK, frame, pos);
            sent = (uint16_t)(sent + got);

            if (got < MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS) {
                break; /* fewer than a full batch means nothing is left (this
                        * also correctly sends the single count=0 frame when
                        * total==0, since the do-while body always runs once) */
            }
        } while (sent < total16);
        break;
    }

    default:
        send_result2(send, io, MC_CH_COMMAND, MC_OP_COMMAND_RESULT, op, MC_RESULT_BAD_REQUEST);
        break;
    }
}

/* --- CONFIG --- */

static void config_send_read(mc_app_t *app, mc_send_fn send, void *io)
{
    char *json = mc_config_to_json(app->config);
    if (json == NULL) {
        uint8_t hdr[4];
        mc_put_u16le(hdr, 0);
        mc_put_u16le(hdr + 2, 0);
        send_frame(send, io, MC_CH_CONFIG, MC_OP_CONFIG_CHUNK, hdr, 4);
        return;
    }
    size_t total = strlen(json);
    size_t off = 0;
    do {
        size_t n = total - off;
        if (n > MC_PROTOCOL_CONFIG_CHUNK) {
            n = MC_PROTOCOL_CONFIG_CHUNK;
        }
        uint8_t frame[4 + MC_PROTOCOL_CONFIG_CHUNK];
        mc_put_u16le(frame, (uint16_t)off);
        mc_put_u16le(frame + 2, (uint16_t)total);
        memcpy(frame + 4, json + off, n);
        send_frame(send, io, MC_CH_CONFIG, MC_OP_CONFIG_CHUNK, frame, 4 + n);
        off += n;
    } while (off < total);
    mc_config_json_free(json);
}

static void config_commit(mc_app_t *app, mc_session_t *s, mc_send_fn send, void *io)
{
    mc_config_t incoming;
    mc_config_result_t pr = mc_config_from_json((const char *)s->cfg_write_buf, s->cfg_write_total, &incoming);
    if (pr != MC_CONFIG_OK) {
        send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
        return;
    }
    if (mc_output_config_validate(&incoming.outputs) != MC_OUT_CFG_OK) {
        send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_REJECTED);
        return;
    }

    /* Apply configuration (functions, names, modes, interlock, input
     * bindings) but preserve each channel's live commanded_on state — a
     * config import must never toggle outputs (AGENTS.md #1). `mode` is NOT
     * forced from the live on/off state here:
     * mode (plain/PWM/flash pattern) is itself part of what a config
     * import is for — e.g. this is how a channel gets opted into
     * MC_OUT_MODE_FLASH_TURN in the first place — so the imported value is
     * kept as-is, same as function/name/pwm_duty_pct. */
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        incoming.outputs.channels[ch].commanded_on = mc_output_get_state(app->output, ch);
    }

    *app->config = incoming;
    app->output->config = incoming.outputs;
    if (app->diag != NULL) {
        /* Same double-bookkeeping as app->output->config above: mc_diag_t
         * carries its own live copy of the thresholds, separate from the
         * persisted mc_config_t.diagnostics a JSON import just replaced. */
        app->diag->config = incoming.diagnostics;
    }

    if (app->persist_config != NULL) {
        app->persist_config(app->app_ctx);
    }
    send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_OK);
}

static void handle_config(mc_session_t *s, mc_app_t *app,
                          const uint8_t *payload, size_t len, mc_send_fn send, void *io)
{
    if (len < 1) {
        return;
    }
    uint8_t op = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = len - 1;

    if (s->state != MC_SESSION_AUTHED) {
        send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_UNAUTHENTICATED);
        return;
    }

    switch (op) {
    case MC_OP_CONFIG_READ:
        config_send_read(app, send, io);
        break;
    case MC_OP_CONFIG_WRITE_BEGIN: {
        if (body_len < 2) {
            send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint16_t total = mc_get_u16le(body);
        if (total == 0 || total > MC_CONFIG_JSON_MAX) {
            send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
            return;
        }
        s->cfg_write_total = total;
        s->cfg_write_got = 0;
        s->cfg_write_active = true;
        break;
    }
    case MC_OP_CONFIG_WRITE_CHUNK: {
        if (!s->cfg_write_active || body_len < 2) {
            send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
            s->cfg_write_active = false;
            return;
        }
        uint16_t offset = mc_get_u16le(body);
        const uint8_t *data = body + 2;
        size_t data_len = body_len - 2;
        if ((size_t)offset + data_len > s->cfg_write_total) {
            send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
            s->cfg_write_active = false;
            return;
        }
        memcpy(s->cfg_write_buf + offset, data, data_len);
        uint16_t end = (uint16_t)(offset + data_len);
        if (end > s->cfg_write_got) {
            s->cfg_write_got = end;
        }
        break;
    }
    case MC_OP_CONFIG_WRITE_COMMIT: {
        if (!s->cfg_write_active || s->cfg_write_got != s->cfg_write_total) {
            send_result1(send, io, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT, MC_RESULT_BAD_REQUEST);
            s->cfg_write_active = false;
            return;
        }
        config_commit(app, s, send, io);
        s->cfg_write_active = false;
        break;
    }
    default:
        break;
    }
}

/* --- OTA (docs/PROTOCOL.md §10) --- */

/* Computed here, not inside mc_ota (which never includes mc_output.h/
 * mc_diag.h — see mc_ota.h's header comment), mirroring how mc_lock_inputs_t
 * is assembled by the platform. A device with no output engine attached
 * (host test fixtures) treats the gate as satisfied rather than permanently
 * blocking OTA. */
static bool ota_safe_to_flash(mc_app_t *app)
{
    if (app->output == NULL) {
        return true;
    }
    return !app->output->engine_running && !mc_output_lv_cutoff_active(app->output);
}

static uint8_t ota_result_to_wire(mc_ota_result_t r)
{
    switch (r) {
    case MC_OTA_OK:
        return MC_RESULT_OK;
    case MC_OTA_ERR_BAD_STATE:
    case MC_OTA_ERR_BAD_SIZE:
    case MC_OTA_ERR_OUT_OF_ORDER_CHUNK:
    case MC_OTA_ERR_OVERRUN:
        return MC_RESULT_BAD_REQUEST;
    case MC_OTA_ERR_FLASH:
        return MC_RESULT_INTERNAL;
    case MC_OTA_ERR_BAD_SIGNATURE:
    case MC_OTA_ERR_HASH_MISMATCH:
    case MC_OTA_ERR_UNSAFE_STATE:
    default:
        return MC_RESULT_REJECTED;
    }
}

static void handle_ota(mc_session_t *s, mc_app_t *app,
                       const uint8_t *payload, size_t len, mc_send_fn send, void *io)
{
    if (len < 1) {
        return;
    }
    uint8_t op = payload[0];
    const uint8_t *body = payload + 1;
    size_t body_len = len - 1;

    if (s->state != MC_SESSION_AUTHED) {
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_UNAUTHENTICATED);
        return;
    }
    if (app->ota == NULL) {
        if (op == MC_OP_OTA_STATUS) {
            uint8_t resp[10] = {0};
            resp[0] = MC_RESULT_REJECTED;
            send_frame(send, io, MC_CH_OTA, MC_OP_OTA_STATUS_RESULT, resp, sizeof(resp));
            return;
        }
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_REJECTED);
        return;
    }

    switch (op) {
    case MC_OP_OTA_BEGIN: {
        size_t needed = 4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES;
        if (body_len != needed) {
            send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint32_t image_size = mc_get_u32le(body);
        const uint8_t *sha512 = body + 4;
        const uint8_t *signature = body + 4 + MC_CRYPTO_HASH_BYTES;
        mc_ota_result_t r = mc_ota_begin(app->ota, image_size, sha512, signature, ota_safe_to_flash(app));
        if (r == MC_OTA_OK && app->log_event != NULL) {
            app->log_event(app->app_ctx, MC_EVT_OTA_BEGIN, 0, 0);
        } else if (r == MC_OTA_ERR_BAD_SIGNATURE && app->log_event != NULL) {
            /* Only the security-relevant rejection is logged — a routine
             * unsafe-state rejection (bike moving, battery low) would just
             * be noisy on retry, unlike this file's other "REJECTED"
             * outcomes which are all similarly non-security-noteworthy. */
            app->log_event(app->app_ctx, MC_EVT_OTA_FAILURE, (uint8_t)r, 0);
        }
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, ota_result_to_wire(r));
        break;
    }
    case MC_OP_OTA_CHUNK: {
        if (body_len < 4) {
            send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_BAD_REQUEST);
            return;
        }
        uint32_t offset = mc_get_u32le(body);
        const uint8_t *data = body + 4;
        size_t data_len = body_len - 4;
        mc_ota_result_t r = mc_ota_chunk(app->ota, offset, data, data_len);
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, ota_result_to_wire(r));
        break;
    }
    case MC_OP_OTA_COMMIT: {
        mc_ota_result_t r = mc_ota_commit(app->ota);
        if (app->log_event != NULL) {
            if (r == MC_OTA_OK) {
                app->log_event(app->app_ctx, MC_EVT_OTA_SUCCESS, 0, 0);
            } else {
                app->log_event(app->app_ctx, MC_EVT_OTA_FAILURE, (uint8_t)r, 0);
            }
        }
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, ota_result_to_wire(r));
        break;
    }
    case MC_OP_OTA_ABORT: {
        mc_ota_abort(app->ota);
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_OK);
        break;
    }
    case MC_OP_OTA_REBOOT: {
        /* On success hal.reboot() doesn't return on real hardware; the sim
         * / test HALs may return normally, so this reply still gets sent
         * there (harmless — nothing depends on it arriving on real hw). */
        mc_ota_result_t r = mc_ota_reboot(app->ota, ota_safe_to_flash(app));
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, ota_result_to_wire(r));
        break;
    }
    case MC_OP_OTA_STATUS: {
        uint8_t resp[10];
        resp[0] = MC_RESULT_OK;
        resp[1] = (uint8_t)mc_ota_get_state(app->ota);
        mc_put_u32le(&resp[2], mc_ota_get_bytes_received(app->ota));
        mc_put_u32le(&resp[6], mc_ota_get_image_size(app->ota));
        send_frame(send, io, MC_CH_OTA, MC_OP_OTA_STATUS_RESULT, resp, sizeof(resp));
        break;
    }
    default:
        send_result1(send, io, MC_CH_OTA, MC_OP_OTA_RESULT, MC_RESULT_BAD_REQUEST);
        break;
    }
}

/* --- dispatch --- */

void mc_session_handle(mc_session_t *s, mc_app_t *app,
                       mc_channel_t ch, const uint8_t *payload, size_t len,
                       mc_send_fn send, void *io)
{
    switch (ch) {
    case MC_CH_STATUS:
        handle_status(s, app, payload, len, send, io);
        break;
    case MC_CH_AUTH:
        handle_auth(s, app, payload, len, send, io);
        break;
    case MC_CH_COMMAND:
        handle_command(s, app, payload, len, send, io);
        break;
    case MC_CH_CONFIG:
        handle_config(s, app, payload, len, send, io);
        break;
    case MC_CH_OTA:
        handle_ota(s, app, payload, len, send, io);
        break;
    default:
        break;
    }
}
