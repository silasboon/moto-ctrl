#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "mc_config.h"
#include "mc_diag.h"
#include "mc_event_log.h"
#include "mc_input.h"
#include "mc_keystore.h"
#include "mc_lock.h"
#include "mc_ota.h"
#include "mc_ota_release_key.h"
#include "mc_output.h"
#include "mc_persist.h"
#include "mc_session.h"
#include "mc_status.h"

#include "ble/ble_app.h"
#include "ble/gatt_svr.h"
#include "diag_hal.h"
#include "factory_reset.h"
#include "input_hal_gpio.h"
#include "nvs_calib_hal.h"
#include "nvs_config_hal.h"
#include "nvs_event_log_hal.h"
#include "nvs_keystore_hal.h"
#include "nvs_lock_hal.h"
#include "ota_hal.h"
#include "output_hal_gpio.h"
#include "watchdog.h"

static const char *TAG = "moto_ctrl";

/* Shared device state the BLE sessions operate on. */
static mc_output_engine_t s_output;
static mc_config_t s_config;
static mc_keystore_t s_keystore;
static mc_lock_t s_lock;
static mc_diag_t s_diag;
static mc_ota_t s_ota;
static mc_event_log_t s_event_log;
static mc_app_t s_app;
static mc_input_engine_t s_input;

/* Debounced-write schedulers respecting NVS flash wear (AGENTS.md). Lock
 * state is NOT debounced here — see persist_lock_cb: lock/cheat-code
 * changes are rare and security-relevant enough to persist immediately. */
static mc_persist_scheduler_t s_persist_config;
static mc_persist_scheduler_t s_persist_keystore;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void persist_config_cb(void *ctx)
{
    (void)ctx;
    mc_persist_mark_dirty(&s_persist_config, now_ms());
}

static void persist_keystore_cb(void *ctx)
{
    (void)ctx;
    mc_persist_mark_dirty(&s_persist_keystore, now_ms());
}

/* Called synchronously (not debounced) right after any mutating lock op —
 * see mc_lock.h and mc_session.h's persist_lock hook doc comment. May run
 * on the NimBLE host task (a BLE-triggered lock command) rather than
 * app_task; app_task's own loop also flushes s_lock.dirty for
 * tick-driven transitions (auto-lock, auto-unlock) that don't go through
 * a command handler at all. */
static void persist_lock_cb(void *ctx)
{
    (void)ctx;
    if (nvs_lock_save(&s_lock) == ESP_OK) {
        mc_lock_clear_dirty(&s_lock);
    }
}

/* MC_OP_DIAG_SET_CALIB persists immediately, like persist_lock_cb — a rare,
 * deliberate installer/bench action, not a hot path (mc_session.h). */
static void persist_diag_calib_cb(void *ctx)
{
    (void)ctx;
    nvs_calib_save(&s_diag.calib);
}

/* Appends one event log record. Platform (not mc_session, which
 * has no clock by design) supplies uptime_ms, same doctrine as
 * fill_status_cb()'s st->uptime_ms below. */
static void log_event_cb(void *app_ctx, uint8_t type, uint8_t arg0, uint8_t arg1)
{
    (void)app_ctx;
    mc_event_log_append(&s_event_log, (mc_event_type_t)type, arg0, arg1, now_ms());
}

/* mc_app_t.on_session_authed: fired once by mc_session.c exactly when a
 * session's challenge-response newly succeeds. Auto-unlocks (edge-
 * triggered, not polled — see mc_lock.h) if the bike is currently LOCKED
 * and the PHONE method is enabled; a no-op otherwise. May run on the
 * NimBLE host task, same as any other BLE-triggered lock mutation. */
static void on_session_authed_cb(void *ctx)
{
    (void)ctx;
    bool was_locked = (s_lock.state == MC_LOCK_ST_LOCKED);
    mc_lock_request_unlock(&s_lock, &s_output, now_ms());
    /* Only actually writes flash if the bike was really LOCKED and this
     * call released it (mc_lock_is_dirty()) — a no-op unlock attempt
     * (already unlocked, or PHONE method disabled) never marks dirty, so
     * routine "phone connects while riding" auth doesn't cost a write. */
    if (mc_lock_is_dirty(&s_lock)) {
        persist_lock_cb(NULL);
    }
    if (was_locked && s_lock.state != MC_LOCK_ST_LOCKED) {
        log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_PHONE_AUTO, 0);
    }
}

static void fill_status_cb(mc_status_t *st, void *ctx)
{
    (void)ctx;
    st->uptime_ms = now_ms();
    st->rssi_dbm = 0;    /* filled once per-connection RSSI is wired */
    /* lock_state + the backoff bit are filled by mc_session's build_status
     * directly from app->lock (same as output_state_mask from app->output);
     * battery_mv + output_fault_mask are filled the same way from app->diag
     * — not this platform callback's job. */
}

/* Does `list` bind to output channel `ch`, either directly (the reserved
 * 256+N range) or via a function id that currently resolves to it? */
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
            if (mc_output_find_indicator_channel(&s_config.outputs, side) == (int)ch) {
                return true;
            }
        }
    }
    return false;
}

/* True if channel `ch` is configured momentary — i.e. driven by button LEVEL
 * in momentary_tick() below, so the press-event path must leave it alone or a
 * single press would both hold it (level) and toggle it (event). */
static bool channel_is_momentary(uint8_t ch)
{
    return ch < MC_OUTPUT_COUNT &&
           s_config.outputs.channels[ch].behaviour == MC_OUT_BEHAVIOUR_MOMENTARY;
}

/* Drives every momentary channel from the level of whichever button's
 * short-press binding targets it: on while held, off on release. Same
 * pass-through pattern as brake_switch_input (mc_output.h), just generalised
 * to any channel. Several buttons may target one channel — any of them held
 * holds the output.
 *
 * Called every tick BEFORE the press-event queue is drained, so a release is
 * honoured promptly rather than a tick late. */
static void momentary_tick(uint32_t t)
{
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        if (s_config.outputs.channels[ch].behaviour != MC_OUT_BEHAVIOUR_MOMENTARY) {
            continue;
        }
        /* A momentary output follows a HOLD, never a tap: a single/double
         * tap binds to toggle/blink instead, so waiting for the hold
         * threshold keeps a tap from blipping the output. */
        bool held = false;
        for (uint8_t b = 0; b < MC_INPUT_COUNT && !held; b++) {
            if (binding_targets_channel(&s_config.inputs.long_press_actions[b], ch) &&
                mc_input_hold_active(&s_input, b)) {
                held = true;
            }
        }
        /* ...or a chord that is still being held down. */
        for (uint8_t i = 0; i < s_config.inputs.combo_count && !held; i++) {
            if (s_config.inputs.combos[i].type == MC_COMBO_CHORD &&
                binding_targets_channel(&s_config.inputs.combos[i].actions, ch) &&
                mc_input_chord_held(&s_input, i)) {
                held = true;
            }
        }
        if (held != mc_output_get_state(&s_output, ch)) {
            /* MC_OUT_SRC_LOCAL: this is the hardware-button path, so a
             * momentary starter binding is permitted here where the app's
             * remote SET_OUTPUT stays blocked (AGENTS.md #6). The interlock
             * and engine-running guards still apply inside mc_output_set(),
             * and a rejected "on" simply leaves the channel off. */
            mc_output_set(&s_output, ch, held, MC_OUT_SRC_LOCAL);
        }
    }
    (void)t;
}

/* Executes one action binding. Shared by short/long/double press and by the
 * generic combos[] matcher so every binding path enforces identical policy:
 * every output change goes through mc_output_set() with MC_OUT_SRC_LOCAL,
 * which is where the immobilizer, the starter remote/engine-running/interlock
 * guards (AGENTS.md #6) and turn mutual exclusion actually live. Returns
 * true if `action` was recognised. */
static bool dispatch_action(mc_action_id_t action, uint32_t t)
{
    if (action == MC_ACTION_NONE) {
        return false;
    }

    /* Direct channel binding (mc_types.h): toggle output N. */
    if (action >= MC_ACTION_OUTPUT_TOGGLE_BASE &&
        action < MC_ACTION_OUTPUT_TOGGLE_BASE + MC_OUTPUT_COUNT) {
        uint8_t ch = (uint8_t)(action - MC_ACTION_OUTPUT_TOGGLE_BASE);
        /* momentary_tick() owns this channel — toggling it here too would
         * fight the level and leave it inverted. */
        if (channel_is_momentary(ch)) {
            return true;
        }
        mc_output_set(&s_output, ch, !mc_output_get_state(&s_output, ch), MC_OUT_SRC_LOCAL);
        return true;
    }

    switch (action) {
    case MC_ACTION_TURN_L_TOGGLE:
    case MC_ACTION_TURN_R_TOGGLE: {
        mc_indicator_side_t side =
            (action == MC_ACTION_TURN_L_TOGGLE) ? MC_INDICATOR_LEFT : MC_INDICATOR_RIGHT;
        int ch = mc_output_find_indicator_channel(&s_config.outputs, side);
        if (ch >= 0 && !channel_is_momentary((uint8_t)ch)) {
            mc_output_set(&s_output, (uint8_t)ch, !mc_output_get_state(&s_output, (uint8_t)ch),
                          MC_OUT_SRC_LOCAL);
        }
        return true;
    }
    case MC_ACTION_HAZARD_TOGGLE:
        mc_output_hazard_press(&s_output, t);
        return true;
    default:
        return false;
    }
}

/* Runs every action in a binding, in list order. One trigger may drive
 * several outputs (mc_types.h's mc_action_list_t). */
static void dispatch_action_list(const mc_action_list_t *list, uint32_t t)
{
    for (uint8_t i = 0; i < list->count && i < MC_ACTION_LIST_MAX; i++) {
        dispatch_action(list->actions[i], t);
    }
}

/* Picks the binding for a press type. All three arrays are persisted and
 * round-tripped through the CONFIG channel (docs/PROTOCOL.md); before this
 * existed only short_press_action was ever read, so long/double bindings
 * were silently accepted and ignored. */
static const mc_action_list_t *actions_for_press(uint8_t button, mc_press_event_type_t type)
{
    static const mc_action_list_t none = { .count = 0 };
    if (button >= MC_INPUT_COUNT) {
        return &none;
    }
    switch (type) {
    case MC_PRESS_SHORT:  return &s_config.inputs.short_press_actions[button];
    case MC_PRESS_LONG:   return &s_config.inputs.long_press_actions[button];
    case MC_PRESS_DOUBLE: return &s_config.inputs.double_press_actions[button];
    default:              return &none;
    }
}

static void app_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    bool raw[BOARD_INPUT_COUNT];
    for (;;) {
        uint32_t t = now_ms();
        input_hal_gpio_sample(raw);
        mc_input_poll(&s_input, t, raw);

        /* Ticked first so mc_diag's mc_output_get_actual_state() calls this
         * same tick already see the current blink phase, and so
         * mc_lock sees this same tick's freshest engine_running
         * (voltage-derived, AGENTS.md #6) when it evaluates its
         * parked-detection guard (AGENTS.md #2). */
        mc_output_tick(&s_output, t);

        /* Brake-lever/pedal switch pass-through: a maintained
         * level, not a press event, mirroring the ignition-switch-mode read
         * below. Deliberately just an ordinary mc_output_set() — the
         * attention-burst pattern (if configured) is purely a property of
         * the BRAKE channel's mode, not of how the transition arrived. */
        int8_t brake_in = s_config.outputs.brake_switch_input;
        if (brake_in >= 0) {
            int brake_ch = mc_output_find_brake_channel(&s_config.outputs);
            if (brake_ch >= 0) {
                bool asserted = mc_input_button_level(&s_input, (uint8_t)brake_in);
                if (asserted != mc_output_get_state(&s_output, (uint8_t)brake_ch)) {
                    mc_output_set(&s_output, (uint8_t)brake_ch, asserted, MC_OUT_SRC_LOCAL);
                }
            }
        }

        /* Momentary channels (horn, starter, hold-to-run aux) follow button
         * level, not press events — run before the event queue is drained. */
        momentary_tick(t);

        bool was_lv_cutoff = mc_output_lv_cutoff_active(&s_output);
        mc_diag_tick(&s_diag, &s_output, t);
        bool now_lv_cutoff = mc_output_lv_cutoff_active(&s_output);
        if (now_lv_cutoff && !was_lv_cutoff) {
            log_event_cb(NULL, MC_EVT_LV_CUTOFF_ENTER, 0, 0);
        } else if (!now_lv_cutoff && was_lv_cutoff) {
            log_event_cb(NULL, MC_EVT_LV_CUTOFF_EXIT, 0, 0);
        }

        int8_t isw = s_lock.config.ignition_switch_input;
        mc_lock_inputs_t lock_inputs = {
            .ignition_switch_level = (isw >= 0) ? mc_input_button_level(&s_input, (uint8_t)isw) : false,
            .engine_running = s_output.engine_running,
        };
        /* mc_lock_tick() drives auto-lock (grace timer) and
         * passive ignition-switch unlock -- the only two LOCKED-state
         * transitions it can cause. Explicit MC_OP_LOCK/UNLOCK are already
         * logged in mc_session.c; phone-auto-unlock in on_session_authed_cb
         * above; cheat-code match/lockout right below. */
        bool was_locked = (s_lock.state == MC_LOCK_ST_LOCKED);
        mc_lock_tick(&s_lock, &s_output, t, &lock_inputs);
        bool now_locked = (s_lock.state == MC_LOCK_ST_LOCKED);
        if (now_locked && !was_locked) {
            log_event_cb(NULL, MC_EVT_LOCK_ENGAGED, 0, 0);
        } else if (was_locked && !now_locked) {
            log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_IGNITION_SWITCH, 0);
        }

        mc_input_event_t evt;
        while (mc_input_pop_event(&s_input, &evt)) {
            if (evt.kind == MC_INPUT_EVT_PRESS) {
                ESP_LOGI(TAG, "button %u press type %d", evt.data.press.button, evt.data.press.type);
                /* Button-identification learn mode (mc_protocol.h). A no-op
                 * unless an authenticated app asked for it, so this costs
                 * nothing while riding. */
                gatt_svr_push_input_event(evt.data.press.button, (uint8_t)evt.data.press.type,
                                          evt.data.press.action_suppressed);
                /* Only short presses feed the cheat-code — see mc_lock.h
                 * (matches mc_input's own sequence-combo matcher, which
                 * also only consumes short presses). A no-op unless the
                 * bike is currently LOCKED. */
                if (evt.data.press.type == MC_PRESS_SHORT) {
                    bool was_backoff = s_lock.backoff_active;
                    mc_lock_cheatcode_outcome_t co =
                        mc_lock_cheatcode_press(&s_lock, &s_output, t, evt.data.press.button);
                    if (co == MC_LOCK_CHEATCODE_MATCH) {
                        log_event_cb(NULL, MC_EVT_LOCK_RELEASED, MC_EVT_UNLOCK_CHEATCODE, 0);
                    } else if (co == MC_LOCK_CHEATCODE_MISMATCH && !was_backoff && s_lock.backoff_active) {
                        /* backoff_active can only transition false->true
                         * inside this same call — see mc_lock.h. */
                        uint16_t wrong = mc_lock_wrong_attempt_count(&s_lock);
                        uint8_t arg0 = (wrong > 0xFF) ? 0xFF : (uint8_t)wrong;
                        log_event_cb(NULL, MC_EVT_CHEATCODE_LOCKOUT, arg0, 0);
                    }
                }

                /* Handlebar bindings, for all three press types. Orthogonal
                 * to the cheat-code above — mc_lock_cheatcode_press() only
                 * ever acts while LOCKED, so a button configured for both an
                 * action and (incidentally) part of the cheat-code buffer
                 * safely does both.
                 *
                 * action_suppressed means a chord containing this button
                 * already fired, so only the chord's actions run — see
                 * mc_input.h. Note the cheat-code feed above deliberately
                 * ignores that flag (AGENTS.md #3). */
                if (!evt.data.press.action_suppressed) {
                    dispatch_action_list(actions_for_press(evt.data.press.button, evt.data.press.type), t);
                }
            } else {
                /* The generic combos[] mechanism (chord/sequence bound to
                 * an arbitrary action_id) is orthogonal to the lock
                 * cheat-code, which mc_lock buffers independently from raw
                 * short-press events — see mc_lock.h. A chord is how
                 * "press both turn buttons together to toggle hazards" is
                 * expressed: combos[] entry of type chord, action_id
                 * MC_ACTION_HAZARD_TOGGLE. */
                ESP_LOGI(TAG, "combo %u (%u action(s))", evt.data.combo.combo_index,
                         evt.data.combo.actions.count);
                dispatch_action_list(&evt.data.combo.actions, t);
            }
        }

        /* Catch every commanded_on change that needs persisting — BLE
         * SET_OUTPUT/HAZARD_PRESS (mc_session.c, running on the NimBLE host
         * task, picked up here within one ~10ms tick), the local brake/turn/
         * hazard dispatch above, and a turn signal's auto-cancel timer
         * expiring inside mc_output_tick() with no command involved at all
         * — a single diff-and-sync catches all of these uniformly rather
         * than needing a persist call at every individual mutation site
         * (AGENTS.md #1: outputs must restore from persisted state, so
         * every one of these has to actually reach NVS). s_output.config is
         * the live truth (mc_output_init() copied it out of s_config.outputs
         * once at boot; they've diverged on every commanded_on change since). */
        if (memcmp(&s_config.outputs, &s_output.config, sizeof(s_output.config)) != 0) {
            s_config.outputs = s_output.config;
            persist_config_cb(NULL);
        }

        /* Flush debounced persistence when due. */
        if (mc_persist_should_flush(&s_persist_config, t)) {
            if (mc_config_save(nvs_config_hal_get(), &s_config) == MC_CONFIG_OK) {
                mc_persist_mark_flushed(&s_persist_config);
            }
        }
        if (mc_persist_should_flush(&s_persist_keystore, t)) {
            if (nvs_keystore_save(&s_keystore) == ESP_OK) {
                mc_persist_mark_flushed(&s_persist_keystore);
            }
        }
        /* Lock state is never debounced — flush immediately whenever a
         * tick-driven transition (auto-lock grace, phone/ignition-switch
         * auto-unlock, or the boot ride-safe override) marked it dirty. */
        if (mc_lock_is_dirty(&s_lock)) {
            persist_lock_cb(NULL);
        }

        mc_watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    /* Must be first: GPIO3/GPIO46 strapping-pin safety before any GPIO use. */
    board_config_early_init();
    /* docs/TESTING.md: CI's QEMU boot job greps stdout for these
     * "MOTO-CTRL boot:" markers to confirm the actual cross-compiled binary
     * reaches each milestone on emulated hardware — QEMU doesn't emulate the
     * NimBLE radio, so the markers bracket that gap rather than assume it. */
    ESP_LOGI(TAG, "MOTO-CTRL boot: early_init done");

    ESP_LOGI(TAG, "MOTO-CTRL firmware %d.%d.%d — board: %s",
             MC_FW_VERSION_MAJOR, MC_FW_VERSION_MINOR, MC_FW_VERSION_PATCH, BOARD_CONFIG_NAME);

    output_hal_gpio_init();
    input_hal_gpio_init();
    diag_hal_init();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_LOGI(TAG, "MOTO-CTRL boot: NVS init done");

    /* AGENTS.md #3: hold BOOT for 10s at power-on to factory reset. Must run
     * before config/keystore/lock are loaded below, so a confirmed reset
     * takes effect before anything reads the blobs it just wiped. A normal
     * boot (BOOT not held) returns immediately — no delay added. */
    (void)factory_reset_check();

    /* Config + keystore: degrade to defaults/empty on any NVS failure rather
     * than aborting (AGENTS.md #1 — never let an error path drop outputs). */
    mc_config_default(&s_config);
    if (nvs_config_hal_init() == ESP_OK) {
        mc_config_result_t r = mc_config_load(nvs_config_hal_get(), &s_config);
        if (r != MC_CONFIG_OK) {
            ESP_LOGE(TAG, "config load failed (%d); using defaults", r);
            mc_config_default(&s_config);
        }
    } else {
        ESP_LOGE(TAG, "config NVS unavailable; using defaults");
    }

    mc_keystore_init(&s_keystore);
    if (nvs_keystore_hal_init() == ESP_OK) {
        if (nvs_keystore_load(&s_keystore) != ESP_OK) {
            ESP_LOGW(TAG, "keystore load failed; starting with no enrolled keys");
        }
    } else {
        ESP_LOGE(TAG, "keystore NVS unavailable; no enrolled keys");
    }

    /* Safety-critical: restore outputs from persisted state ASAP after boot
     * (AGENTS.md #1, <250ms). */
    mc_output_init(&s_output, &s_config.outputs, output_hal_gpio_get());
    mc_output_restore_from_config(&s_output);
    ESP_LOGI(TAG, "outputs restored from persisted config");
    ESP_LOGI(TAG, "MOTO-CTRL boot: outputs restored");

    /* Lock state: degrade to disabled/unlocked on any NVS failure, same
     * doctrine as config/keystore above — never guess into an immobilized
     * boot. Must run after mc_output_restore_from_config() (mc_lock_init
     * reads whether ignition is currently live, per AGENTS.md #1's
     * ride-safe restore override) and before app_task starts. */
    mc_lock_config_t lock_cfg;
    bool locked_flag = false;
    mc_lock_config_default(&lock_cfg);
    if (nvs_lock_hal_init() == ESP_OK) {
        if (nvs_lock_load(&lock_cfg, &locked_flag) != ESP_OK) {
            ESP_LOGW(TAG, "lock state load failed; starting disabled/unlocked");
            mc_lock_config_default(&lock_cfg);
            locked_flag = false;
        }
    } else {
        ESP_LOGE(TAG, "lock NVS unavailable; immobilizer stays disabled");
    }
    mc_lock_init(&s_lock, &lock_cfg, locked_flag, &s_output, now_ms());
    if (mc_lock_is_dirty(&s_lock)) {
        /* The ride-safe boot override fired (see mc_lock_init) — persist
         * the corrected state immediately rather than waiting for the
         * first app_task tick. */
        if (nvs_lock_save(&s_lock) == ESP_OK) {
            mc_lock_clear_dirty(&s_lock);
        }
    }
    ESP_LOGI(TAG, "lock state restored (wire state %d)", (int)mc_lock_wire_state(&s_lock));
    ESP_LOGI(TAG, "MOTO-CTRL boot: lock state restored");

    /* Board calibration: degrade to nominal/uncalibrated defaults on any NVS
     * failure, same doctrine as config/keystore/lock above. Never security-
     * or ride-safety-critical by itself (mc_diag_tick's own hysteresis and
     * the battery_mv==0 "unknown, don't cut off" guard bound the worst case
     * of a bad calibration), so this degrades quietly rather than logging
     * at error level. Thresholds (mc_diag_config_t) come from s_config.
     * diagnostics, already loaded above as part of s_config. */
    mc_diag_calib_t diag_calib;
    mc_diag_calib_default(&diag_calib);
    if (nvs_calib_hal_init() == ESP_OK) {
        if (nvs_calib_load(&diag_calib) != ESP_OK) {
            ESP_LOGW(TAG, "calibration load failed; using nominal/uncalibrated defaults");
            mc_diag_calib_default(&diag_calib);
        }
    } else {
        ESP_LOGW(TAG, "calibration NVS unavailable; using nominal/uncalibrated defaults");
    }
    mc_diag_init(&s_diag, &s_config.diagnostics, &diag_calib, diag_hal_get());
    ESP_LOGI(TAG, "MOTO-CTRL boot: diagnostics engine ready");

    mc_input_init(&s_input, &s_config.inputs);
    ESP_LOGI(TAG, "MOTO-CTRL boot: input engine ready");

    /* OTA. ota_hal_init() starts the dedicated low-priority flash-
     * write task (ota_hal.c); mc_ota_init() itself does no I/O. */
    ota_hal_init();
    mc_ota_init(&s_ota, ota_hal_get(), MC_OTA_RELEASE_PUBKEY);
    ESP_LOGI(TAG, "MOTO-CTRL boot: OTA ready");

    /* Event log, on its own dedicated "evtlog" NVS partition —
     * degrades to an empty (but still functional in-RAM) log on any NVS
     * failure, same doctrine as config/keystore/lock/calib above; a broken
     * event log must never become a reason to fail boot or drop outputs. */
    if (nvs_event_log_hal_init() != ESP_OK) {
        ESP_LOGE(TAG, "event log NVS unavailable; history will not persist this boot");
    }
    mc_event_log_init(&s_event_log, nvs_event_log_hal_get());
    ESP_LOGI(TAG, "MOTO-CTRL boot: event log ready");

    mc_persist_init(&s_persist_config, 2000);
    mc_persist_init(&s_persist_keystore, 2000);

    s_app.output = &s_output;
    s_app.config = &s_config;
    s_app.keystore = &s_keystore;
    s_app.lock = &s_lock;
    s_app.diag = &s_diag;
    s_app.ota = &s_ota;
    s_app.event_log = &s_event_log;
    s_app.input = &s_input;
    s_app.fill_status = fill_status_cb;
    s_app.persist_config = persist_config_cb;
    s_app.persist_keystore = persist_keystore_cb;
    s_app.persist_lock = persist_lock_cb;
    s_app.persist_diag_calib = persist_diag_calib_cb;
    s_app.on_session_authed = on_session_authed_cb;
    s_app.log_event = log_event_cb;
    s_app.app_ctx = NULL;

    mc_watchdog_init();
    ESP_LOGI(TAG, "MOTO-CTRL boot: watchdog init done");

    ESP_LOGI(TAG, "MOTO-CTRL boot: starting BLE stack");
    ble_app_start(&s_app);
    ESP_LOGI(TAG, "MOTO-CTRL boot: BLE stack started");

    xTaskCreate(app_task, "mc_app", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "MOTO-CTRL boot: app_main complete");
}
