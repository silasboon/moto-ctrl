#include "sim_debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ws_server.h"

/* Event log ring buffer: process-global (one simulator instance per
 * process), independent of ctx->lock so a GET_LOG replay doesn't block
 * device-state handling. */
typedef struct {
    uint32_t t_ms;
    char text[SIM_LOG_TEXT_MAX];
} sim_log_entry_t;

static sim_log_entry_t g_log[SIM_LOG_RING_LEN];
static int g_log_head = 0;
static int g_log_count = 0;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void push_frame(uint8_t op, const uint8_t *payload, size_t plen)
{
    uint8_t buf[8 + SIM_LOG_TEXT_MAX];
    if (plen + 2 > sizeof(buf)) {
        return;
    }
    buf[0] = SIM_CH_DEBUG;
    buf[1] = op;
    if (plen > 0) {
        memcpy(buf + 2, payload, plen);
    }
    ws_server_send_to_active(buf, plen + 2);
}

static void push_log_entry(uint32_t t_ms, const char *text)
{
    uint8_t payload[4 + 1 + SIM_LOG_TEXT_MAX];
    payload[0] = (uint8_t)(t_ms & 0xFF);
    payload[1] = (uint8_t)((t_ms >> 8) & 0xFF);
    payload[2] = (uint8_t)((t_ms >> 16) & 0xFF);
    payload[3] = (uint8_t)((t_ms >> 24) & 0xFF);
    uint8_t tlen = (uint8_t)strlen(text);
    if (tlen > SIM_LOG_TEXT_MAX - 1) {
        tlen = SIM_LOG_TEXT_MAX - 1;
    }
    payload[4] = tlen;
    memcpy(payload + 5, text, tlen);
    push_frame(SIM_OP_LOG_ENTRY, payload, 5u + tlen);
}

void sim_debug_logf(sim_debug_ctx_t *ctx, const char *fmt, ...)
{
    char text[SIM_LOG_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    uint32_t t = ctx->now_ms();

    pthread_mutex_lock(&g_log_lock);
    int idx = (g_log_head + g_log_count) % SIM_LOG_RING_LEN;
    if (g_log_count < SIM_LOG_RING_LEN) {
        g_log_count++;
    } else {
        g_log_head = (g_log_head + 1) % SIM_LOG_RING_LEN;
    }
    g_log[idx].t_ms = t;
    strncpy(g_log[idx].text, text, SIM_LOG_TEXT_MAX - 1);
    g_log[idx].text[SIM_LOG_TEXT_MAX - 1] = '\0';
    pthread_mutex_unlock(&g_log_lock);

    printf("[sim] %s\n", text);
    fflush(stdout);
    push_log_entry(t, text);
}

static void replay_log(void)
{
    pthread_mutex_lock(&g_log_lock);
    sim_log_entry_t snapshot[SIM_LOG_RING_LEN];
    int count = g_log_count;
    for (int i = 0; i < count; i++) {
        snapshot[i] = g_log[(g_log_head + i) % SIM_LOG_RING_LEN];
    }
    pthread_mutex_unlock(&g_log_lock);
    for (int i = 0; i < count; i++) {
        push_log_entry(snapshot[i].t_ms, snapshot[i].text);
    }
}

void sim_debug_init(sim_debug_ctx_t *ctx,
                    mc_output_engine_t *output, mc_input_engine_t *input,
                    mc_config_t *config, mc_keystore_t *keystore, mc_lock_t *lock, mc_diag_t *diag, sim_nvs_t *nvs,
                    mc_persist_scheduler_t *persist_config, mc_persist_scheduler_t *persist_keystore,
                    uint32_t (*now_ms)(void))
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->output = output;
    ctx->input = input;
    ctx->config = config;
    ctx->keystore = keystore;
    ctx->mc_lock = lock;
    ctx->diag = diag;
    ctx->nvs = nvs;
    ctx->persist_config = persist_config;
    ctx->persist_keystore = persist_keystore;
    ctx->now_ms = now_ms;
    pthread_mutex_init(&ctx->lock, NULL);
    ctx->battery_mv = 13200; /* plausible resting charged LiFePO4 -- below the
                              * default engine_run_mv (13800), so engine_running
                              * stays false by default; see mc_diag.h */
    for (int c = 0; c < MC_OUTPUT_COUNT; c++) {
        ctx->channel_fault[c] = SIM_FAULT_NONE;
    }
}

void sim_debug_lock(sim_debug_ctx_t *ctx) { pthread_mutex_lock(&ctx->lock); }
void sim_debug_unlock(sim_debug_ctx_t *ctx) { pthread_mutex_unlock(&ctx->lock); }

static void send_state(sim_debug_ctx_t *ctx)
{
    /* battery/current/fault/engine_running are read from mc_diag /
     * mc_output -- the same real, computed values STATUS reports over the
     * actual protocol (mc_session's build_status()) -- rather than the raw
     * injected fields, so GET_STATE and STATUS_GET can never disagree. */
    uint16_t battery_mv = (ctx->diag != NULL) ? mc_diag_get_battery_mv(ctx->diag) : ctx->battery_mv;
    bool engine_running = ctx->output->engine_running;

    uint8_t p[4 + MC_OUTPUT_COUNT * 3 + 2];
    size_t o = 0;
    p[o++] = (uint8_t)(battery_mv & 0xFF);
    p[o++] = (uint8_t)((battery_mv >> 8) & 0xFF);
    p[o++] = engine_running ? 1 : 0;
    p[o++] = ctx->interlock_engaged ? 1 : 0;
    for (int c = 0; c < MC_OUTPUT_COUNT; c++) {
        uint16_t cur = (ctx->diag != NULL) ? ctx->diag->current_ma[c] : ctx->channel_current_ma[c];
        mc_diag_fault_t fault = (ctx->diag != NULL) ? ctx->diag->fault[c] : MC_DIAG_FAULT_NONE;
        p[o++] = (uint8_t)(cur & 0xFF);
        p[o++] = (uint8_t)((cur >> 8) & 0xFF);
        p[o++] = (uint8_t)fault;
    }
    p[o++] = ctx->mc_lock != NULL ? (uint8_t)mc_lock_wire_state(ctx->mc_lock) : (uint8_t)MC_LOCK_UNKNOWN;
    p[o++] = ctx->mc_lock != NULL && mc_lock_backoff_active(ctx->mc_lock) ? 1 : 0;
    push_frame(SIM_OP_STATE, p, o);
}

/* Simulates an MCU reset: reloads config + keystore from the fake NVS
 * (exercising the same fallback-to-defaults path as firmware/main/main.c's
 * app_main() on real NVS-read failure) and re-runs
 * mc_output_restore_from_config(), timed against the AGENTS.md #1 <250ms
 * restore budget. Then drops the connection, since a real reboot drops the
 * BLE link. */
static void force_reboot(sim_debug_ctx_t *ctx)
{
    pthread_mutex_lock(&ctx->lock);

    sim_debug_logf(ctx, "reboot: simulating MCU reset (watchdog/brownout)");

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    mc_config_store_hal_t cfg_hal = {
        .load = sim_nvs_config_load,
        .save = sim_nvs_config_save,
        .ctx = ctx->nvs,
    };
    mc_config_t new_config;
    mc_config_default(&new_config);
    mc_config_result_t cr = mc_config_load(cfg_hal, &new_config);
    if (cr != MC_CONFIG_OK) {
        sim_debug_logf(ctx, "reboot: config load failed (err=%d); falling back to defaults", (int)cr);
        mc_config_default(&new_config);
    }
    *ctx->config = new_config;

    mc_keystore_t new_keystore;
    mc_keystore_init(&new_keystore);
    if (!sim_nvs_keystore_load(ctx->nvs, &new_keystore)) {
        sim_debug_logf(ctx, "reboot: keystore load failed or absent; starting with no enrolled keys");
        mc_keystore_init(&new_keystore);
    }
    *ctx->keystore = new_keystore;

    /* Safety-critical: restore outputs from persisted state ASAP (AGENTS.md
     * #1, <250ms). Re-use the existing hal (same fake GPIO sink). */
    mc_output_hal_t hal = ctx->output->hal;
    mc_output_init(ctx->output, &ctx->config->outputs, hal);
    mc_output_restore_from_config(ctx->output);

    /* Lock state: mirrors firmware/main/main.c's boot order — load after
     * output restore (mc_lock_init reads whether ignition is live) and
     * before the timing measurement ends, so a persisted LOCKED state is
     * also covered by the <250ms budget. */
    if (ctx->mc_lock != NULL) {
        mc_lock_config_t lock_cfg;
        bool locked_flag = false;
        if (!sim_nvs_lock_load(ctx->nvs, &lock_cfg, &locked_flag)) {
            sim_debug_logf(ctx, "reboot: lock state load failed; falling back to disabled/unlocked");
        }
        mc_lock_init(ctx->mc_lock, &lock_cfg, locked_flag, ctx->output, ctx->now_ms());
        if (mc_lock_is_dirty(ctx->mc_lock)) {
            /* Ride-safe boot override fired: persist immediately, same as
             * a real boot would (main.c does this inline too). */
            if (sim_nvs_lock_save(ctx->nvs, ctx->mc_lock)) {
                mc_lock_clear_dirty(ctx->mc_lock);
            }
        }
    }

    /* Diagnostics: config.diagnostics just reloaded as part of *ctx->config
     * above (it rides mc_config_t, same as outputs/inputs); reload
     * calibration from its own blob (mirrors lock's own-blob reload just
     * above) and re-init mc_diag so its runtime state (current_ma/fault
     * history, engine_running, lv_cutoff) starts clean, same as a real
     * reboot would -- current-sense history from before a watchdog reset
     * isn't meaningful to carry forward. */
    if (ctx->diag != NULL) {
        mc_diag_hal_t diag_hal = ctx->diag->hal;
        mc_diag_calib_t calib;
        if (!sim_nvs_calib_load(ctx->nvs, &calib)) {
            sim_debug_logf(ctx, "reboot: calibration load failed; falling back to sim identity calibration");
        }
        mc_diag_init(ctx->diag, &ctx->config->diagnostics, &calib, diag_hal);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long elapsed_us = (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;

    uint32_t cfg_debounce = ctx->persist_config->debounce_ms;
    uint32_t ks_debounce = ctx->persist_keystore->debounce_ms;
    mc_persist_init(ctx->persist_config, cfg_debounce);
    mc_persist_init(ctx->persist_keystore, ks_debounce);

    mc_input_init(ctx->input, &ctx->config->inputs);
    memset(ctx->button_pressed, 0, sizeof(ctx->button_pressed));
    ctx->interlock_engaged = false;

    sim_debug_logf(ctx, "reboot: outputs + lock state restored in %ld us (budget 250000us) -> %s",
                   elapsed_us, elapsed_us < 250000L ? "PASS" : "FAIL");
    if (ctx->mc_lock != NULL) {
        sim_debug_logf(ctx, "reboot: lock wire state = %d", (int)mc_lock_wire_state(ctx->mc_lock));
    }

    pthread_mutex_unlock(&ctx->lock);
    ws_server_close_active();
}

void sim_debug_handle(sim_debug_ctx_t *ctx, const uint8_t *payload, size_t len)
{
    if (len < 1) {
        return;
    }
    uint8_t op = payload[0];
    const uint8_t *body = payload + 1;
    size_t blen = len - 1;

    switch (op) {
    case SIM_OP_GET_STATE: {
        pthread_mutex_lock(&ctx->lock);
        send_state(ctx);
        pthread_mutex_unlock(&ctx->lock);
        return;
    }
    case SIM_OP_GET_LOG:
        replay_log();
        return;
    case SIM_OP_FORCE_DISCONNECT:
        sim_debug_logf(ctx, "forced BLE disconnect");
        ws_server_close_active();
        return;
    case SIM_OP_FORCE_REBOOT:
        force_reboot(ctx);
        return;
    default:
        break;
    }

    pthread_mutex_lock(&ctx->lock);
    switch (op) {
    case SIM_OP_SET_BATTERY_MV:
        if (blen >= 2) {
            ctx->battery_mv = (uint16_t)(body[0] | ((uint16_t)body[1] << 8));
            sim_debug_logf(ctx, "battery set to %u mV", ctx->battery_mv);
        }
        break;
    case SIM_OP_SET_CHANNEL_FAULT:
        /* `fault` (body[3]) is accepted for wire compatibility but ignored
         * -- see sim_protocol.h. Only current_ma feeds the real mc_diag
         * classification (mc_diag_hal reads ctx->channel_current_ma[]). */
        if (blen >= 4 && body[0] < MC_OUTPUT_COUNT) {
            uint8_t ch = body[0];
            ctx->channel_current_ma[ch] = (uint16_t)(body[1] | ((uint16_t)body[2] << 8));
            ctx->channel_fault[ch] = (sim_fault_t)body[3];
            sim_debug_logf(ctx, "channel %u: current=%umA (fault will be derived by mc_diag from thresholds)",
                           ch, ctx->channel_current_ma[ch]);
        }
        break;
    case SIM_OP_SET_ENGINE_RUNNING:
        /* mc_diag derives engine_running for real from
         * ctx->battery_mv every tick and pushes it via
         * mc_output_set_engine_running() itself -- calling that directly
         * here would just be overwritten by the next tick. Nudge the
         * injected battery voltage instead, comfortably on the requested
         * side of the configured threshold (with its hysteresis), so the
         * real derivation lands where asked. */
        if (blen >= 1 && ctx->diag != NULL) {
            bool want_running = body[0] != 0;
            uint16_t threshold = ctx->diag->config.engine_run_mv;
            uint16_t margin = (uint16_t)(ctx->diag->config.engine_run_hysteresis_mv + 200u);
            ctx->battery_mv = want_running ? (uint16_t)(threshold + margin)
                                            : (uint16_t)((threshold > margin) ? (threshold - margin) : 0);
            sim_debug_logf(ctx, "engine_running=%s requested -> battery set to %u mV (mc_diag derives it for real)",
                           want_running ? "true" : "false", ctx->battery_mv);
        }
        break;
    case SIM_OP_SET_INTERLOCK:
        if (blen >= 1) {
            ctx->interlock_engaged = body[0] != 0;
            mc_output_set_interlock_engaged(ctx->output, ctx->interlock_engaged);
            sim_debug_logf(ctx, "starter interlock engaged=%s", ctx->interlock_engaged ? "true" : "false");
        }
        break;
    case SIM_OP_BUTTON_STATE:
        if (blen >= 2 && body[0] < MC_INPUT_COUNT) {
            ctx->button_pressed[body[0]] = body[1] != 0;
        }
        break;
    case SIM_OP_FORCE_NVS_CORRUPT: {
        sim_nvs_target_t t = (blen >= 1) ? (sim_nvs_target_t)body[0] : SIM_NVS_TARGET_BOTH;
        if (t == SIM_NVS_TARGET_CONFIG || t == SIM_NVS_TARGET_BOTH) {
            sim_nvs_corrupt(&ctx->nvs->config);
        }
        if (t == SIM_NVS_TARGET_KEYSTORE || t == SIM_NVS_TARGET_BOTH) {
            sim_nvs_corrupt(&ctx->nvs->keystore);
        }
        if (t == SIM_NVS_TARGET_LOCK) {
            sim_nvs_corrupt(&ctx->nvs->lock);
        }
        if (t == SIM_NVS_TARGET_CALIB) {
            sim_nvs_corrupt(&ctx->nvs->calib);
        }
        sim_debug_logf(ctx, "fake NVS corrupted (target=%d); effect appears on next Force Reboot", (int)t);
        break;
    }
    case SIM_OP_RESET_FAULTS:
        /* battery_mv reset below is below the default engine_run_mv
         * threshold, so mc_diag will derive engine_running=false on its own
         * next tick -- no direct mc_output_set_engine_running() call needed
         * (mc_diag_tick is the sole owner of that flag, mc_diag.h). */
        ctx->battery_mv = 13200;
        ctx->interlock_engaged = false;
        mc_output_set_interlock_engaged(ctx->output, false);
        for (int c = 0; c < MC_OUTPUT_COUNT; c++) {
            ctx->channel_current_ma[c] = 0;
            ctx->channel_fault[c] = SIM_FAULT_NONE;
        }
        sim_debug_logf(ctx, "fault/sensor overrides reset to defaults");
        break;
    default:
        break;
    }
    pthread_mutex_unlock(&ctx->lock);

    uint8_t ack[2] = { op, 1 };
    push_frame(SIM_OP_ACK, ack, 2);
}
