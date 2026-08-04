/*
 * MOTO-CTRL host simulator entry point.
 *
 * Runs the real portable core (mc_session + engines) behind a WebSocket
 * "fake BLE" transport so the app and CI can exercise the full protocol
 * (enrollment, challenge-response auth, control, config) with no hardware.
 * Each WebSocket message is [channel byte][opcode][payload], mirroring the
 * BLE GATT characteristic-per-channel layout (see docs/PROTOCOL.md).
 *
 * Also includes: a real (poll-driven) mc_input engine wired to virtual
 * buttons, a fake NVS blob store (sim_nvs.h) so config/keystore actually
 * persist and can be corrupted, and a 10ms ticker thread that plays the
 * role of firmware/main/main.c's app_task — polling input, flushing
 * debounced persistence, and pushing periodic status frames. The debug/
 * fault-injection channel (sim_debug.h, sim_protocol.h) that the browser
 * GUI (firmware/sim/gui/) drives is layered on top without touching
 * components/core — see docs/TESTING.md.
 *
 * State is per-process: a simulated reboot (SIM_OP_FORCE_REBOOT) reloads
 * config/keystore from the fake NVS, exactly like a real reboot reloads
 * from flash; a real process restart starts with empty fake NVS, keeping
 * CI runs hermetic.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mc_diag.h"
#include "mc_input.h"
#include "mc_lock.h"
#include "mc_persist.h"
#include "mc_session.h"
#include "sim_debug.h"
#include "sim_nvs.h"
#include "sim_protocol.h"
#include "ws_server.h"

static mc_output_engine_t g_output;
static mc_input_engine_t g_input;
static mc_config_t g_config;
static mc_keystore_t g_keystore;
static mc_lock_t g_lock;
static mc_diag_t g_diag;
static mc_ota_t g_ota;
static mc_event_log_t g_event_log;
static mc_app_t g_app;
static sim_nvs_t g_nvs;
static sim_debug_ctx_t g_dbg;
static mc_persist_scheduler_t g_persist_config;
static mc_persist_scheduler_t g_persist_keystore;

static uint32_t g_boot_ms;

/* A FIXED (not randomly generated) Ed25519 test keypair -- so that
 * Node itest / app Jest tooling can hardcode the matching secret key and
 * sign test OTA images that this sim's compiled-in public key will accept,
 * without ever touching the real maintainer release key
 * (firmware/components/core/mc_ota_release_key.c, real-target only). Never
 * used on real hardware. Generated once via the same host-only
 * mc_crypto_keypair() mc_lock's tests already use to stand in for the
 * phone -- see tools/README.md / docs/TESTING.md for the mirrored value. */
static const uint8_t SIM_OTA_TEST_PUBKEY[MC_CRYPTO_PUBKEY_BYTES] = {
    0x53, 0x7c, 0xde, 0xa3, 0xcc, 0x7e, 0xe4, 0x52,
    0xda, 0xd1, 0xc2, 0x36, 0x66, 0x97, 0x0d, 0x73,
    0x0f, 0x9c, 0x9e, 0xfe, 0x8e, 0xee, 0xbf, 0x39,
    0x43, 0x76, 0x5e, 0xe0, 0xf2, 0x07, 0xd4, 0x73,
};

/* Fake flash: an in-memory buffer standing in for esp_ota_*'s inactive
 * partition. Sized to MC_OTA_MAX_IMAGE_SIZE, same ceiling mc_ota.c itself
 * enforces on image_size. */
static uint8_t g_ota_flash_buf[MC_OTA_MAX_IMAGE_SIZE];
static uint32_t g_ota_flash_len;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void sim_output_set(uint8_t channel, bool on, void *ctx)
{
    (void)ctx;
    printf("[output] channel %u -> %s\n", channel, on ? "ON" : "OFF");
    fflush(stdout);
}

/* No real PWM hardware to model in the sim — this just makes the
 * duty value observable (console + GUI-visible via config readback) so the
 * wire/config path is exercised end to end, the same spirit as
 * sim_output_set() above not modeling real electrical behavior either. */
static void sim_output_set_duty(uint8_t channel, uint8_t duty_pct, void *ctx)
{
    (void)ctx;
    printf("[output] channel %u -> PWM %u%%\n", channel, duty_pct);
    fflush(stdout);
}

/* --- OTA flash HAL (fake, in-memory) + reboot --- */

static bool sim_ota_flash_begin(uint32_t image_size, void *ctx)
{
    (void)ctx;
    g_ota_flash_len = 0;
    sim_debug_logf(&g_dbg, "OTA: begin, image_size=%u", image_size);
    return image_size <= sizeof(g_ota_flash_buf);
}
static bool sim_ota_flash_write(uint32_t offset, const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if ((uint64_t)offset + (uint64_t)len > sizeof(g_ota_flash_buf)) {
        return false;
    }
    memcpy(g_ota_flash_buf + offset, data, len);
    if (offset + len > g_ota_flash_len) {
        g_ota_flash_len = offset + (uint32_t)len;
    }
    return true;
}
static bool sim_ota_flash_finalize(void *ctx)
{
    (void)ctx;
    sim_debug_logf(&g_dbg, "OTA: finalize, %u bytes", g_ota_flash_len);
    return true;
}
static void sim_ota_flash_abort(void *ctx)
{
    (void)ctx;
    g_ota_flash_len = 0;
    sim_debug_logf(&g_dbg, "OTA: aborted");
}
/* No real second firmware image to switch to in a single compiled sim
 * binary — this is protocol-level only, proving the begin/chunk/commit/
 * reboot wire sequence completes, not "the device now runs new code."
 * See docs/TESTING.md's OTA caveat. */
static void sim_ota_reboot(void *ctx)
{
    (void)ctx;
    sim_debug_logf(&g_dbg, "OTA: reboot requested (sim: protocol-level only, no real image swap)");
}

/* Appends one event log record. Platform (not mc_session, which
 * has no clock by design) supplies uptime_ms, same doctrine as
 * sim_fill_status()'s st->uptime_ms below. */
static void log_event_cb(void *app_ctx, uint8_t type, uint8_t arg0, uint8_t arg1)
{
    (void)app_ctx;
    mc_event_log_append(&g_event_log, (mc_event_type_t)type, arg0, arg1, now_ms() - g_boot_ms);
}

/* Caller must hold g_dbg.lock (mc_session's status handler always runs
 * under it — see on_message / the ticker loop below). */
static void sim_fill_status(mc_status_t *st, void *app_ctx)
{
    (void)app_ctx;
    st->uptime_ms = now_ms() - g_boot_ms;
    st->rssi_dbm = -55; /* no real radio in the sim; a plausible constant */
    /* lock_state + the backoff bit are filled by mc_session's build_status
     * directly from g_app.lock; battery_mv + output_fault_mask
     * the same way from g_app.diag — not this callback's job. */
}

/* mc_diag_hal_t for the sim: reads back injected values (g_dbg.channel_
 * current_ma[]/battery_mv) rather than real analog hardware. The real
 * mc_diag threshold/calibration/cutoff/engine-running logic (mc_diag.c,
 * compiled and tested identically to the on-target build) then runs on top
 * of these unmodified — see sim_nvs.h's doc comment on why the sim's
 * DEFAULT calibration (not this HAL) is what makes the numbers line up
 * 1:1 with injected values out of the box. */
static uint16_t sim_diag_read_channel_mv(uint8_t channel, void *ctx)
{
    sim_debug_ctx_t *dbg = (sim_debug_ctx_t *)ctx;
    if (channel >= MC_OUTPUT_COUNT) {
        return 0;
    }
    return dbg->channel_current_ma[channel];
}

static uint16_t sim_diag_read_vbat_mv(void *ctx)
{
    sim_debug_ctx_t *dbg = (sim_debug_ctx_t *)ctx;
    return dbg->battery_mv;
}

/* Mirrors binding_targets_channel()/channel_is_momentary()/momentary_tick()
 * in firmware/main/main.c. Momentary channels follow button LEVEL rather than
 * latching on a press event — see mc_output.h's momentary field. */
static bool binding_targets_channel(const mc_action_list_t *list, uint8_t ch)
{
    for (uint8_t i = 0; i < list->count && i < MC_ACTION_LIST_MAX; i++) {
        mc_action_id_t a = list->actions[i];
        if (a >= MC_ACTION_OUTPUT_TOGGLE_BASE &&
            a - MC_ACTION_OUTPUT_TOGGLE_BASE == ch) {
            return true;
        }
        if (a == MC_ACTION_TURN_L_TOGGLE || a == MC_ACTION_TURN_R_TOGGLE) {
            mc_indicator_side_t side =
                (a == MC_ACTION_TURN_L_TOGGLE) ? MC_INDICATOR_LEFT : MC_INDICATOR_RIGHT;
            if (mc_output_find_indicator_channel(&g_config.outputs, side) == (int)ch) {
                return true;
            }
        }
    }
    return false;
}

static bool channel_is_momentary(uint8_t ch)
{
    return ch < MC_OUTPUT_COUNT &&
           g_config.outputs.channels[ch].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY;
}

/* Mirrors gatt_svr_input_actions_suppressed(): an app capturing a cheat-code
 * asks for its presses not to fire bindings. Declared here, defined below the
 * session pointer it reads. */
static bool input_actions_suppressed(void);

static void momentary_tick(void)
{
    /* Level-driven, so skipping press dispatch isn't enough to keep these
     * quiet during a cheat-code capture — see firmware/main/main.c. */
    bool suppressed = input_actions_suppressed();

    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (g_config.outputs.channels[ch].behaviour != MC_OUT_BEHAVIOUR_MOMENTARY) {
            continue;
        }
        /* A momentary output follows a HOLD, never a tap: a single/double
         * tap binds to toggle/blink instead, so waiting for the hold
         * threshold keeps a tap from blipping the output. */
        bool held = false;
        for (uint8_t b = 0; b < MC_INPUT_COUNT && !held && !suppressed; b++) {
            if (binding_targets_channel(&g_config.inputs.long_press_actions[b], ch) &&
                mc_input_hold_active(&g_input, b)) {
                held = true;
            }
        }
        /* ...or a chord that is still being held down. */
        for (uint8_t i = 0; i < g_config.inputs.combo_count && !held && !suppressed; i++) {
            if (g_config.inputs.combos[i].type == MC_COMBO_CHORD &&
                binding_targets_channel(&g_config.inputs.combos[i].actions, ch) &&
                mc_input_chord_held(&g_input, i)) {
                held = true;
            }
        }
        if (held != mc_output_get_state(&g_output, ch)) {
            mc_output_set(&g_output, ch, held, MC_OUT_SRC_LOCAL);
        }
    }
}

/* Mirrors dispatch_action()/action_for_press() in firmware/main/main.c
 * exactly — the sim and the target must bind buttons identically or the
 * scenario replay tests in docs/TESTING.md stop proving anything about real
 * firmware behaviour. */
static bool dispatch_action(mc_action_id_t action, uint32_t t)
{
    if (action == MC_ACTION_NONE) {
        return false;
    }

    if (action >= MC_ACTION_OUTPUT_TOGGLE_BASE &&
        action < MC_ACTION_OUTPUT_TOGGLE_BASE + MC_OUTPUT_COUNT) {
        uint8_t ch = (uint8_t)(action - MC_ACTION_OUTPUT_TOGGLE_BASE);
        if (channel_is_momentary(ch)) {
            return true; /* momentary_tick() owns it */
        }
        mc_output_set(&g_output, ch, !mc_output_get_state(&g_output, ch), MC_OUT_SRC_LOCAL);
        return true;
    }

    if (action >= MC_ACTION_OUTPUT_ALTERNATE_BASE &&
        action < MC_ACTION_OUTPUT_ALTERNATE_BASE + MC_OUTPUT_COUNT) {
        uint8_t ch = (uint8_t)(action - MC_ACTION_OUTPUT_ALTERNATE_BASE);
        int8_t partner = g_config.outputs.channels[ch].alternate_channel;
        if (channel_is_momentary(ch) ||
            (partner >= 0 && partner < MC_OUTPUT_COUNT && channel_is_momentary((uint8_t)partner))) {
            return true; /* momentary_tick() owns it */
        }
        mc_output_alternate_press(&g_output, ch, MC_OUT_SRC_LOCAL);
        return true;
    }

    switch (action) {
    case MC_ACTION_TURN_L_TOGGLE:
    case MC_ACTION_TURN_R_TOGGLE: {
        mc_indicator_side_t side =
            (action == MC_ACTION_TURN_L_TOGGLE) ? MC_INDICATOR_LEFT : MC_INDICATOR_RIGHT;
        int ch = mc_output_find_indicator_channel(&g_config.outputs, side);
        if (ch >= 0 && !channel_is_momentary((uint8_t)ch)) {
            mc_output_set(&g_output, (uint8_t)ch, !mc_output_get_state(&g_output, (uint8_t)ch),
                          MC_OUT_SRC_LOCAL);
        }
        return true;
    }
    case MC_ACTION_HAZARD_TOGGLE:
        mc_output_hazard_press(&g_output, t);
        return true;
    default:
        return false;
    }
}

static void dispatch_action_list(const mc_action_list_t *list, uint32_t t)
{
    for (uint8_t i = 0; i < list->count && i < MC_ACTION_LIST_MAX; i++) {
        dispatch_action(list->actions[i], t);
    }
}

static const mc_action_list_t *actions_for_press(uint8_t button, mc_press_event_type_t type)
{
    static const mc_action_list_t none = { .count = 0 };
    if (button >= MC_INPUT_COUNT) {
        return &none;
    }
    switch (type) {
    case MC_PRESS_SHORT:  return &g_config.inputs.short_press_actions[button];
    case MC_PRESS_LONG:   return &g_config.inputs.long_press_actions[button];
    case MC_PRESS_DOUBLE: return &g_config.inputs.double_press_actions[button];
    default:              return &none;
    }
}

static void persist_config_cb(void *ctx)
{
    (void)ctx;
    mc_persist_mark_dirty(&g_persist_config, now_ms());
}

static void persist_keystore_cb(void *ctx)
{
    (void)ctx;
    mc_persist_mark_dirty(&g_persist_keystore, now_ms());
}

/* Lock state persists immediately, not debounced — see mc_lock.h /
 * firmware/main/main.c's persist_lock_cb for the same rationale. */
static void persist_lock_cb(void *ctx)
{
    (void)ctx;
    if (sim_nvs_lock_save(&g_nvs, &g_lock)) {
        mc_lock_clear_dirty(&g_lock);
    }
}

/* MC_OP_DIAG_SET_CALIB persists immediately, mirroring persist_lock_cb — see
 * mc_session.h's persist_diag_calib doc comment. */
static void persist_diag_calib_cb(void *ctx)
{
    (void)ctx;
    sim_nvs_calib_save(&g_nvs, &g_diag.calib);
}

/* mc_app_t.on_session_authed: fired once by mc_session.c exactly when a
 * session's challenge-response newly succeeds. Mirrors
 * firmware/main/main.c's on_session_authed_cb — see mc_lock.h for why this
 * is edge-triggered rather than a level polled every ticker cycle. Caller
 * (mc_session_handle, from on_message) already holds g_dbg.lock. */
static void on_session_authed_cb(void *ctx)
{
    (void)ctx;
    bool was_locked = (g_lock.state == MC_LOCK_ST_LOCKED);
    mc_lock_request_unlock(&g_lock, &g_output, now_ms());
    if (mc_lock_is_dirty(&g_lock)) {
        persist_lock_cb(NULL);
    }
    if (was_locked && g_lock.state != MC_LOCK_ST_LOCKED) {
        log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_PHONE_AUTO, 0);
    }
}

/* WebSocket -> session: strip the channel byte, dispatch, and let the
 * session's responses go back out prefixed with the channel byte. */
static void ws_send_adapter(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    uint8_t buf[512];
    if (len + 1 > sizeof(buf)) {
        return;
    }
    buf[0] = (uint8_t)ch;
    memcpy(buf + 1, data, len);
    ws_send(io, buf, len + 1);
}

/* Same framing, but pushes to whichever connection is active rather than a
 * specific `io` handle — used for the ticker's unsolicited status pushes. */
static void push_adapter(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    (void)io;
    uint8_t buf[512];
    if (len + 1 > sizeof(buf)) {
        return;
    }
    buf[0] = (uint8_t)ch;
    memcpy(buf + 1, data, len);
    ws_server_send_to_active(buf, len + 1);
}

/* Pushes an unsolicited STATUS frame by round-tripping through the real
 * mc_session status handler (rather than duplicating build_status's logic
 * here), so the periodic push can never drift from what a client's own
 * STATUS_GET would see. Caller must hold g_dbg.lock. */
static void push_status_tick(void)
{
    mc_session_t tmp;
    mc_session_init(&tmp);
    uint8_t op = MC_OP_STATUS_GET;
    mc_session_handle(&tmp, &g_app, MC_CH_STATUS, &op, 1, push_adapter, NULL);
}

/* The single active connection's session, so the ticker thread can check
 * MC_OP_INPUT_LEARN without a `conn` handle — the sim's counterpart to
 * gatt_svr_push_input_event() iterating the real session table. ws_server
 * only ever has one connection at a time (ws_server.h). */
static mc_session_t *g_active_session;

static bool input_actions_suppressed(void)
{
    const mc_session_t *s = g_active_session;
    return s != NULL && s->input_learn_suppress_actions && mc_session_is_authed(s);
}

/* Mirrors gatt_svr_push_input_event(). A no-op unless an authenticated
 * client asked for learn mode. */
static void push_input_event(uint8_t button, uint8_t press_type, bool action_suppressed)
{
    mc_session_t *s = g_active_session;
    if (s == NULL || !s->input_learn || !mc_session_is_authed(s)) {
        return;
    }
    const uint8_t frame[4] = { MC_OP_INPUT_EVENT, button, press_type,
                               (uint8_t)(action_suppressed ? 1 : 0) };
    push_adapter(NULL, MC_CH_COMMAND, frame, sizeof(frame));
}

static void *on_open(void *user)
{
    (void)user;
    mc_session_t *s = malloc(sizeof(mc_session_t));
    if (s != NULL) {
        mc_session_init(s);
        g_active_session = s;
        sim_debug_logf(&g_dbg, "client connected");
    }
    return s;
}

static void on_message(void *user, void *conn, const uint8_t *data, size_t len)
{
    (void)user;
    if (len < 1) {
        return;
    }
    mc_session_t *s = (mc_session_t *)ws_conn_userdata(conn);
    if (s == NULL) {
        return;
    }
    uint8_t ch = data[0];
    if (ch == SIM_CH_DEBUG) {
        /* sim_debug_handle manages its own locking — do not hold g_dbg.lock
         * around it, or a nested lock here would deadlock. */
        sim_debug_handle(&g_dbg, data + 1, len - 1);
        return;
    }
    sim_debug_lock(&g_dbg);
    mc_session_handle(s, &g_app, (mc_channel_t)ch, data + 1, len - 1, ws_send_adapter, conn);
    sim_debug_unlock(&g_dbg);
}

static void on_close(void *user, void *userdata)
{
    (void)user;
    sim_debug_logf(&g_dbg, "client disconnected");
    if (g_active_session == userdata) {
        /* Clear before freeing: learn mode must not outlive the link, and a
         * dangling pointer here would be read by the ticker thread. */
        g_active_session = NULL;
    }
    free(userdata);
}

/* Stands in for firmware/main/main.c's app_task: polls virtual buttons
 * through the real mc_input engine, flushes debounced persistence to the
 * fake NVS, and pushes a status frame a few times a second so the GUI
 * dashboard updates without polling. Runs for the life of the process. */
static void *ticker_thread(void *arg)
{
    (void)arg;
    uint32_t last_status_push = 0;

    for (;;) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10L * 1000L * 1000L };
        nanosleep(&ts, NULL);

        sim_debug_lock(&g_dbg);
        uint32_t t = now_ms();

        bool raw[MC_INPUT_COUNT];
        memcpy(raw, g_dbg.button_pressed, sizeof(raw));
        mc_input_poll(&g_input, t, raw);

        /* Ticked first so mc_diag's mc_output_get_actual_state() calls this
         * same tick already see the current blink phase, and so
         * mc_lock sees this same tick's freshest engine_running
         * (voltage-derived, AGENTS.md #6) — mirrors firmware/main/main.c's
         * app_task ordering. */
        momentary_tick();
        mc_output_tick(&g_output, t);

        /* Brake-lever/pedal switch pass-through — mirrors
         * firmware/main/main.c's app_task exactly (see its comment). */
        int8_t brake_in = g_config.outputs.brake_switch_input;
        if (brake_in >= 0) {
            int brake_ch = mc_output_find_brake_channel(&g_config.outputs);
            if (brake_ch >= 0) {
                bool asserted = mc_input_button_level(&g_input, (uint8_t)brake_in);
                if (asserted != mc_output_get_state(&g_output, (uint8_t)brake_ch)) {
                    mc_output_set(&g_output, (uint8_t)brake_ch, asserted, MC_OUT_SRC_LOCAL);
                }
            }
        }

        bool was_lv_cutoff = mc_output_lv_cutoff_active(&g_output);
        mc_diag_tick(&g_diag, &g_output, t);
        bool now_lv_cutoff = mc_output_lv_cutoff_active(&g_output);
        if (now_lv_cutoff && !was_lv_cutoff) {
            log_event_cb(NULL, MC_EVT_LV_CUTOFF_ENTER, 0, 0);
        } else if (!now_lv_cutoff && was_lv_cutoff) {
            log_event_cb(NULL, MC_EVT_LV_CUTOFF_EXIT, 0, 0);
        }

        int8_t isw = g_lock.config.ignition_switch_input;
        mc_lock_inputs_t lock_inputs = {
            .ignition_switch_level = (isw >= 0) ? mc_input_button_level(&g_input, (uint8_t)isw) : false,
            .engine_running = g_output.engine_running,
        };
        /* mc_lock_tick() drives auto-lock (grace timer) and
         * passive ignition-switch unlock -- the only two LOCKED-state
         * transitions it can cause. Explicit MC_OP_LOCK/UNLOCK are already
         * logged in mc_session.c; phone-auto-unlock in on_session_authed_cb
         * above; cheat-code match/lockout right below. */
        bool was_locked = (g_lock.state == MC_LOCK_ST_LOCKED);
        mc_lock_tick(&g_lock, &g_output, t, &lock_inputs);
        bool now_locked = (g_lock.state == MC_LOCK_ST_LOCKED);
        if (now_locked && !was_locked) {
            log_event_cb(NULL, MC_EVT_LOCK_ENGAGED, 0, 0);
        } else if (was_locked && !now_locked) {
            log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_IGNITION_SWITCH, 0);
        }

        mc_input_event_t evt;
        while (mc_input_pop_event(&g_input, &evt)) {
            /* Mirrors firmware/main/main.c: only short presses feed the
             * cheat-code (a no-op unless the sim bike is currently
             * LOCKED), while action bindings fire for all three press
             * types and for generic combos. */
            if (evt.kind == MC_INPUT_EVT_PRESS) {
                sim_debug_logf(&g_dbg, "button %u press type %d", evt.data.press.button, (int)evt.data.press.type);
                push_input_event(evt.data.press.button, (uint8_t)evt.data.press.type,
                                 evt.data.press.action_suppressed);
                if (evt.data.press.type == MC_PRESS_SHORT) {
                    bool was_backoff = g_lock.backoff_active;
                    mc_lock_cheatcode_outcome_t co =
                        mc_lock_cheatcode_press(&g_lock, &g_output, t, evt.data.press.button);
                    if (co == MC_LOCK_CHEATCODE_MATCH) {
                        log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_CHEATCODE, 0);
                    } else if (co == MC_LOCK_CHEATCODE_MISMATCH && !was_backoff && g_lock.backoff_active) {
                        /* backoff_active can only transition false->true
                         * inside this same call — a press while already in
                         * backoff short-circuits to IN_BACKOFF before ever
                         * reaching the mismatch path (mc_lock.h). */
                        uint16_t wrong = mc_lock_wrong_attempt_count(&g_lock);
                        uint8_t arg0 = (wrong > 0xFF) ? 0xFF : (uint8_t)wrong;
                        log_event_cb(NULL, MC_EVT_CHEATCODE_LOCKOUT, arg0, 0);
                    }
                }

                /* action_suppressed: a chord containing this button already
                 * fired. The cheat-code feed above ignores it (AGENTS.md
                 * #3); only action dispatch honours it. */
                if (!evt.data.press.action_suppressed &&
                    !input_actions_suppressed()) {
                    dispatch_action_list(actions_for_press(evt.data.press.button, evt.data.press.type), t);
                }
            } else {
                sim_debug_logf(&g_dbg, "combo %u matched (%u action(s))",
                               evt.data.combo.combo_index, evt.data.combo.actions.count);
                dispatch_action_list(&evt.data.combo.actions, t);
            }
        }

        /* Same output-persistence catch-all as firmware/main/main.c's
         * app_task — see its comment for why a single diff-and-sync (rather
         * than a persist call at every mutation site) is needed. */
        if (memcmp(&g_config.outputs, &g_output.config, sizeof(g_output.config)) != 0) {
            g_config.outputs = g_output.config;
            persist_config_cb(NULL);
        }

        if (mc_persist_should_flush(&g_persist_config, t)) {
            mc_config_store_hal_t hal = {
                .load = sim_nvs_config_load, .save = sim_nvs_config_save, .ctx = &g_nvs,
            };
            if (mc_config_save(hal, &g_config) == MC_CONFIG_OK) {
                mc_persist_mark_flushed(&g_persist_config);
            }
        }
        if (mc_persist_should_flush(&g_persist_keystore, t)) {
            if (sim_nvs_keystore_save(&g_nvs, &g_keystore)) {
                mc_persist_mark_flushed(&g_persist_keystore);
            }
        }
        if (mc_lock_is_dirty(&g_lock)) {
            persist_lock_cb(NULL);
        }

        if ((uint32_t)(t - last_status_push) >= 200) {
            last_status_push = t;
            push_status_tick();
        }
        sim_debug_unlock(&g_dbg);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    uint16_t port = 8010;
    if (argc >= 2) {
        port = (uint16_t)atoi(argv[1]);
    }

    g_boot_ms = now_ms();

    sim_nvs_init(&g_nvs);
    mc_config_default(&g_config);
    mc_keystore_init(&g_keystore);
    mc_output_hal_t hal = { .set = sim_output_set, .set_duty = sim_output_set_duty, .ctx = NULL };
    mc_output_init(&g_output, &g_config.outputs, hal);
    mc_input_init(&g_input, &g_config.inputs);

    mc_lock_config_t lock_cfg;
    bool locked_flag = false;
    if (!sim_nvs_lock_load(&g_nvs, &lock_cfg, &locked_flag)) {
        mc_lock_config_default(&lock_cfg);
        locked_flag = false;
    }
    mc_lock_init(&g_lock, &lock_cfg, locked_flag, &g_output, now_ms());
    if (mc_lock_is_dirty(&g_lock)) {
        if (sim_nvs_lock_save(&g_nvs, &g_lock)) {
            mc_lock_clear_dirty(&g_lock);
        }
    }

    mc_persist_init(&g_persist_config, 2000);
    mc_persist_init(&g_persist_keystore, 2000);

    /* sim_nvs_calib_load() always leaves a usable value in diag_calib —
     * either what was persisted, or (absent/corrupt) the sim's own identity
     * calibration (see sim_nvs.h) — so its bool return is purely
     * informational here, unlike sim_nvs_lock_load()'s callers above. */
    mc_diag_calib_t diag_calib;
    (void)sim_nvs_calib_load(&g_nvs, &diag_calib);
    mc_diag_hal_t diag_hal = {
        .read_channel_mv = sim_diag_read_channel_mv,
        .read_vbat_mv = sim_diag_read_vbat_mv,
        .ctx = &g_dbg,
    };
    mc_diag_init(&g_diag, &g_config.diagnostics, &diag_calib, diag_hal);

    mc_ota_hal_t ota_hal = {
        .flash_begin = sim_ota_flash_begin,
        .flash_write = sim_ota_flash_write,
        .flash_finalize = sim_ota_flash_finalize,
        .flash_abort = sim_ota_flash_abort,
        .reboot = sim_ota_reboot,
        .ctx = NULL,
    };
    mc_ota_init(&g_ota, ota_hal, SIM_OTA_TEST_PUBKEY);

    /* Event log's fake storage lives in g_nvs (sim_nvs_evtlog_*), persisting
     * across a simulated reboot (SIM_OP_FORCE_REBOOT) exactly like config/
     * keystore/lock — a fresh process start gets an empty log, same "fake
     * NVS is per-process" doctrine as everything else in sim_nvs.h. */
    mc_event_log_hal_t evt_hal = {
        .read_slot = sim_nvs_evtlog_read_slot,
        .write_slot = sim_nvs_evtlog_write_slot,
        .get_last_seq = sim_nvs_evtlog_get_last_seq,
        .set_last_seq = sim_nvs_evtlog_set_last_seq,
        .ctx = &g_nvs,
    };
    mc_event_log_init(&g_event_log, evt_hal);

    sim_debug_init(&g_dbg, &g_output, &g_input, &g_config, &g_keystore, &g_lock, &g_diag, &g_nvs,
                   &g_persist_config, &g_persist_keystore, now_ms);

    g_app.output = &g_output;
    g_app.config = &g_config;
    g_app.keystore = &g_keystore;
    g_app.lock = &g_lock;
    g_app.diag = &g_diag;
    g_app.ota = &g_ota;
    g_app.event_log = &g_event_log;
    g_app.input = &g_input;
    g_app.fill_status = sim_fill_status;
    g_app.persist_config = persist_config_cb;
    g_app.persist_keystore = persist_keystore_cb;
    g_app.persist_lock = persist_lock_cb;
    g_app.persist_diag_calib = persist_diag_calib_cb;
    g_app.on_session_authed = on_session_authed_cb;
    g_app.log_event = log_event_cb;
    g_app.app_ctx = NULL;

    pthread_t ticker;
    pthread_create(&ticker, NULL, ticker_thread, NULL);

    ws_callbacks_t cb = {
        .on_open = on_open,
        .on_message = on_message,
        .on_close = on_close,
        .user = NULL,
    };
    return ws_server_run(port, &cb);
}
