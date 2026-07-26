/*
 * mc_diag — diagnostics: calibration math, round-robin current
 * sampling, open-load/overcurrent classification, engine_running / low-
 * voltage-cutoff derivation (each with hysteresis), learn, calibration
 * persistence, and the wire-level dispatch of the COMMAND-channel
 * DIAG_* opcodes. Mirrors test_lock.c's fixture/session-fixture style.
 */
#include "mc_diag.h"

#include <assert.h>
#include <string.h>

#include "mc_session.h"

/* --- fake diag HAL: returns directly-injected raw values, no analog model --- */

typedef struct {
    uint16_t channel_raw_mv[MC_OUTPUT_COUNT];
    uint16_t vbat_raw_mv;
} fake_hal_ctx_t;

static uint16_t fake_read_channel_mv(uint8_t channel, void *ctx)
{
    return ((fake_hal_ctx_t *)ctx)->channel_raw_mv[channel];
}

static uint16_t fake_read_vbat_mv(void *ctx)
{
    return ((fake_hal_ctx_t *)ctx)->vbat_raw_mv;
}

static void hal_noop(uint8_t channel, bool on, void *ctx)
{
    (void)channel; (void)on; (void)ctx;
}

/* Identity calibration (is_gain=1, kilis=2000, vbat_gain=1, all offsets 0)
 * so calc_current_ma(mv)==mv and calc_battery_mv(mv)==mv exactly — same
 * trick firmware/sim/src/main.c uses for its default, so tests can inject a
 * "current" or "voltage" number and assert on that same number coming back
 * out, without re-deriving mc_diag.c's private arithmetic here. */
static void identity_calib(mc_diag_calib_t *out)
{
    memset(out, 0, sizeof(*out));
    out->is_gain = 1.0f;
    out->is_offset_mv = 0;
    out->kilis = 2000.0f;
    out->vbat_gain = 1.0f;
    out->vbat_offset_mv = 0;
}

/* --- fixture: a diag engine + the output engine it reads/drives --- */

typedef struct {
    mc_output_engine_t output;
    mc_output_config_t out_cfg;
    mc_diag_t diag;
    mc_diag_config_t diag_cfg;
    mc_diag_calib_t calib;
    fake_hal_ctx_t hal_ctx;
} diag_fixture_t;

static void fx_init(diag_fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    mc_output_config_default(&fx->out_cfg);
    mc_output_hal_t ohal = { .set = hal_noop, .ctx = NULL };
    mc_output_init(&fx->output, &fx->out_cfg, ohal);

    mc_diag_config_default(&fx->diag_cfg);
    identity_calib(&fx->calib);
    mc_diag_hal_t dhal = { .read_channel_mv = fake_read_channel_mv, .read_vbat_mv = fake_read_vbat_mv, .ctx = &fx->hal_ctx };
    mc_diag_init(&fx->diag, &fx->diag_cfg, &fx->calib, dhal);
    fx->hal_ctx.vbat_raw_mv = 13200; /* resting, not charging -- below the default engine_run_mv */
}

/* --- config / calib defaults + validation --- */

static void test_config_default_and_validate(void)
{
    mc_diag_config_t cfg;
    mc_diag_config_default(&cfg);
    assert(mc_diag_config_validate(&cfg) == MC_DIAG_CFG_OK);
    assert(cfg.channels[0].open_load_ma == MC_DIAG_DEFAULT_OPEN_LOAD_MA);
    assert(cfg.channels[MC_OUTPUT_COUNT - 1].overcurrent_ma == MC_DIAG_DEFAULT_OVERCURRENT_MA);
    assert(cfg.lv_cutoff_mv == MC_DIAG_DEFAULT_LV_CUTOFF_MV);
    assert(cfg.engine_run_mv == MC_DIAG_DEFAULT_ENGINE_RUN_MV);
    assert(cfg.engine_run_mv > MC_DIAG_DEFAULT_LV_CUTOFF_MV); /* charging threshold must sit above cutoff */

    cfg.channels[3].open_load_ma = 100;
    cfg.channels[3].overcurrent_ma = 50; /* inverted: never classifiable as overcurrent */
    assert(mc_diag_config_validate(&cfg) & MC_DIAG_CFG_BAD_CHANNEL_THRESHOLDS);
}

static void test_calib_default(void)
{
    mc_diag_calib_t c;
    mc_diag_calib_default(&c);
    assert(c.is_gain == 1.0f);
    assert(c.is_offset_mv == 0);
    assert(c.kilis == 1.0f); /* deliberately not a fabricated datasheet number */
    assert(c.vbat_offset_mv == 0);
    assert(c.vbat_gain > 10.9f && c.vbat_gain < 11.1f); /* ~1/0.0909 */
}

/* --- round-robin sampling --- */

static void test_sampling_skips_non_energized_channels(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    assert(mc_output_set(&fx.output, 2, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[2] = 500;
    fx.hal_ctx.channel_raw_mv[3] = 999; /* channel 3 stays commanded off */

    mc_diag_tick(&fx.diag, &fx.output, 0);

    assert(fx.diag.current_ma[2] == 500);
    assert(fx.diag.fault[2] == MC_DIAG_FAULT_NONE); /* within default thresholds */
    assert(fx.diag.current_ma[3] == 0);
    assert(fx.diag.fault[3] == MC_DIAG_FAULT_NONE);
    assert(fx.diag.fault_mask == 0);
}

static void test_round_robin_one_real_sample_per_tick(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_output_set(&fx.output, 1, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[0] = 111;
    fx.hal_ctx.channel_raw_mv[1] = 222;

    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.diag.current_ma[0] == 111);
    assert(fx.diag.current_ma[1] == 0); /* round-robin cursor hasn't reached it yet */

    mc_diag_tick(&fx.diag, &fx.output, 10);
    assert(fx.diag.current_ma[1] == 222);
}

/* --- fault classification --- */

static void test_fault_classification(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.diag.config.channels[0].open_load_ma = 100;
    fx.diag.config.channels[0].overcurrent_ma = 1000;

    fx.hal_ctx.channel_raw_mv[0] = 50; /* below open_load */
    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_OPEN_LOAD);
    assert(fx.diag.fault_mask & (1u << 0));

    fx.hal_ctx.channel_raw_mv[0] = 500; /* healthy */
    mc_diag_tick(&fx.diag, &fx.output, 10);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_NONE);
    assert(!(fx.diag.fault_mask & (1u << 0)));

    fx.hal_ctx.channel_raw_mv[0] = 5000; /* above overcurrent */
    mc_diag_tick(&fx.diag, &fx.output, 20);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_OVERCURRENT);
    assert(fx.diag.fault_mask & (1u << 0));
}

static void test_lv_cutoff_suppressed_channel_never_faulted(void)
{
    /* A channel suppressed by the low-voltage cutoff is not "actually
     * energized" (mc_output_get_actual_state()), so mc_diag must treat it
     * exactly like a commanded-off channel: 0 current, never faulted --
     * not an open-load fault just because the cutoff turned it off. */
    diag_fixture_t fx;
    fx_init(&fx);
    fx.out_cfg.channels[0].function = MC_OUT_FUNC_HORN; /* non-essential */
    /* re-init output with the updated function assignment */
    mc_output_hal_t ohal = { .set = hal_noop, .ctx = NULL };
    mc_output_init(&fx.output, &fx.out_cfg, ohal);
    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    mc_output_set_lv_cutoff(&fx.output, true);
    fx.hal_ctx.channel_raw_mv[0] = 5; /* would be an open-load reading if sampled */

    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.diag.current_ma[0] == 0);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_NONE);
}

static void test_flash_turn_off_phase_never_faulted(void)
{
    /* Cross-module correctness point: a MC_OUT_MODE_FLASH_TURN
     * channel's off-phase must read as "not actually energized" to
     * mc_diag, exactly like the lv_cutoff-suppressed case above --
     * otherwise every other blink cycle would misclassify as an open-load
     * fault. See mc_output.h/mc_output_get_actual_state()'s doc
     * comment: it accounts for blink phase, not just commanded_on. */
    diag_fixture_t fx;
    fx_init(&fx);
    fx.out_cfg.channels[0].function = MC_OUT_FUNC_TURN_L;
    fx.out_cfg.channels[0].mode = MC_OUT_MODE_FLASH_TURN;
    fx.out_cfg.turn_flash_period_ms = 200; /* half-period = 100ms */
    mc_output_hal_t ohal = { .set = hal_noop, .ctx = NULL };
    mc_output_init(&fx.output, &fx.out_cfg, ohal);
    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[0] = 5; /* would be an open-load reading if sampled */

    /* now_ms=100 falls in the off half of the blink cycle. */
    mc_diag_tick(&fx.diag, &fx.output, 100);
    assert(fx.diag.current_ma[0] == 0);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_NONE);

    /* now_ms=1000 falls in the on half -- now it's genuinely sampled and
     * correctly classified as open-load. */
    mc_diag_tick(&fx.diag, &fx.output, 1000);
    assert(fx.diag.current_ma[0] == 5);
    assert(fx.diag.fault[0] == MC_DIAG_FAULT_OPEN_LOAD);
}

/* --- learn --- */

static void test_learn_sets_open_load_from_measured_draw(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    assert(mc_output_set(&fx.output, 4, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[4] = 400;

    assert(mc_diag_learn(&fx.diag, &fx.output, 4) == true);
    assert(fx.diag.config.channels[4].open_load_ma == 200); /* half of measured */
    /* overcurrent_ma is untouched by learn -- a single healthy sample says
     * nothing about a safe upper bound. */
    assert(fx.diag.config.channels[4].overcurrent_ma == MC_DIAG_DEFAULT_OVERCURRENT_MA);

    fx.diag.config.channels[4].open_load_ma = 0;
    assert(mc_diag_learn(&fx.diag, &fx.output, 0xFF) == true); /* learn-all still finds it */
    assert(fx.diag.config.channels[4].open_load_ma == 200);

    assert(mc_diag_learn(&fx.diag, &fx.output, 5) == false); /* channel 5 is off */

    assert(mc_output_set(&fx.output, 4, false, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    assert(mc_diag_learn(&fx.diag, &fx.output, 0xFF) == false); /* nothing energized at all */

    assert(mc_diag_learn(&fx.diag, &fx.output, 200) == false); /* bad channel index */
}

/* --- engine_running derivation + hysteresis --- */

static void test_engine_running_derivation_and_hysteresis(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    fx.diag.config.engine_run_mv = 13800;
    fx.diag.config.engine_run_hysteresis_mv = 300;

    fx.hal_ctx.vbat_raw_mv = 13200;
    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.output.engine_running == false);

    fx.hal_ctx.vbat_raw_mv = 13800; /* at threshold: engages */
    mc_diag_tick(&fx.diag, &fx.output, 10);
    assert(fx.output.engine_running == true);

    fx.hal_ctx.vbat_raw_mv = 13600; /* dropped, but within the hysteresis band */
    mc_diag_tick(&fx.diag, &fx.output, 20);
    assert(fx.output.engine_running == true);

    fx.hal_ctx.vbat_raw_mv = 13400; /* below 13800-300=13500: clears */
    mc_diag_tick(&fx.diag, &fx.output, 30);
    assert(fx.output.engine_running == false);
}

/* --- low-voltage cutoff --- */

static void test_lv_cutoff_engages_and_recovers_with_hysteresis(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    fx.diag.config.lv_cutoff_mv = 11800;
    fx.diag.config.lv_cutoff_hysteresis_mv = 300;
    fx.diag.config.engine_run_mv = 13800; /* keep engine_running false throughout */

    fx.hal_ctx.vbat_raw_mv = 12000;
    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.output.lv_cutoff == false);

    fx.hal_ctx.vbat_raw_mv = 11700; /* below cutoff */
    mc_diag_tick(&fx.diag, &fx.output, 10);
    assert(fx.output.lv_cutoff == true);

    fx.hal_ctx.vbat_raw_mv = 12000; /* above cutoff but within the hysteresis band (< 12100) */
    mc_diag_tick(&fx.diag, &fx.output, 20);
    assert(fx.output.lv_cutoff == true);

    fx.hal_ctx.vbat_raw_mv = 12200; /* above 11800+300=12100: recovers */
    mc_diag_tick(&fx.diag, &fx.output, 30);
    assert(fx.output.lv_cutoff == false);
}

static void test_lv_cutoff_gated_on_not_running(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    /* Deliberately inverted vs realistic defaults, purely to isolate
     * "cutoff never engages while engine_running" from the ordinary
     * threshold comparison (with realistic defaults, engine_run_mv always
     * sits well above lv_cutoff_mv, so this combination can't naturally
     * arise -- see mc_diag.h's default comments). */
    fx.diag.config.engine_run_mv = 5000;
    fx.diag.config.engine_run_hysteresis_mv = 100;
    fx.diag.config.lv_cutoff_mv = 20000;
    fx.diag.config.lv_cutoff_hysteresis_mv = 100;

    fx.hal_ctx.vbat_raw_mv = 10000; /* >= engine_run_mv, and < lv_cutoff_mv */
    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.output.engine_running == true);
    assert(fx.output.lv_cutoff == false);
}

static void test_zero_battery_reading_does_not_trigger_cutoff(void)
{
    diag_fixture_t fx;
    fx_init(&fx);
    fx.hal_ctx.vbat_raw_mv = 0; /* unread/failed ADC line, not a real 0V battery */
    mc_diag_tick(&fx.diag, &fx.output, 0);
    assert(fx.diag.battery_mv == 0);
    assert(fx.output.lv_cutoff == false);
}

/* --- apply / accessors --- */

static void test_apply_config_and_calib(void)
{
    diag_fixture_t fx;
    fx_init(&fx);

    mc_diag_config_t newcfg;
    mc_diag_config_default(&newcfg);
    newcfg.lv_cutoff_mv = 12000;
    mc_diag_apply_config(&fx.diag, &newcfg);
    assert(fx.diag.config.lv_cutoff_mv == 12000);

    mc_diag_calib_t newcal;
    mc_diag_calib_default(&newcal);
    newcal.kilis = 42.0f;
    mc_diag_apply_calib(&fx.diag, &newcal);
    assert(fx.diag.calib.kilis == 42.0f);

    fx.diag.battery_mv = 12345;
    fx.diag.fault_mask = 0x0F0F;
    assert(mc_diag_get_battery_mv(&fx.diag) == 12345);
    assert(mc_diag_get_fault_mask(&fx.diag) == 0x0F0F);
}

/* --- calibration persistence --- */

static void test_calib_serialize_roundtrip(void)
{
    mc_diag_calib_t c;
    mc_diag_calib_default(&c);
    c.is_gain = 2.5f;
    c.is_offset_mv = -7;
    c.kilis = 1700.0f;
    c.vbat_gain = 11.5f;
    c.vbat_offset_mv = 3;

    uint8_t buf[128];
    size_t len = 0;
    assert(mc_diag_calib_serialize(&c, buf, sizeof(buf), &len) == MC_DIAG_STORE_OK);

    mc_diag_calib_t out;
    assert(mc_diag_calib_deserialize(buf, len, &out) == MC_DIAG_STORE_OK);
    assert(out.is_gain == 2.5f);
    assert(out.is_offset_mv == -7);
    assert(out.kilis == 1700.0f);
    assert(out.vbat_gain == 11.5f);
    assert(out.vbat_offset_mv == 3);

    uint8_t tiny[4];
    size_t tlen = 0;
    assert(mc_diag_calib_serialize(&c, tiny, sizeof(tiny), &tlen) == MC_DIAG_STORE_ERR_BUFFER_TOO_SMALL);

    buf[0] ^= 0xFF; /* corrupt magic */
    assert(mc_diag_calib_deserialize(buf, len, &out) == MC_DIAG_STORE_ERR_CORRUPT);
    assert(mc_diag_calib_deserialize(buf, 3, &out) == MC_DIAG_STORE_ERR_CORRUPT); /* truncated */
}

/* --- wire-level (mc_session) dispatch --- */

typedef struct {
    mc_channel_t ch;
    uint8_t data[80];
    size_t len;
} rec_frame_t;

typedef struct {
    rec_frame_t frames[32];
    int count;
} recorder_t;

static void rec_send(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    recorder_t *r = (recorder_t *)io;
    assert(r->count < 32);
    assert(len <= sizeof(r->frames[0].data));
    r->frames[r->count].ch = ch;
    memcpy(r->frames[r->count].data, data, len);
    r->frames[r->count].len = len;
    r->count++;
}

static void rec_reset(recorder_t *r) { r->count = 0; }

static const rec_frame_t *last_frame(const recorder_t *r, mc_channel_t ch, uint8_t opcode)
{
    for (int i = r->count - 1; i >= 0; i--) {
        if (r->frames[i].ch == ch && r->frames[i].len >= 1 && r->frames[i].data[0] == opcode) {
            return &r->frames[i];
        }
    }
    return NULL;
}

typedef struct {
    mc_output_engine_t output;
    mc_config_t config;
    mc_keystore_t keystore;
    mc_diag_t diag;
    mc_diag_calib_t calib;
    fake_hal_ctx_t hal_ctx;
    mc_app_t app;
    int persist_config_calls;
    int persist_diag_calib_calls;
} diag_session_fixture_t;

static void dsfx_persist_config(void *ctx) { ((diag_session_fixture_t *)ctx)->persist_config_calls++; }
static void dsfx_persist_calib(void *ctx) { ((diag_session_fixture_t *)ctx)->persist_diag_calib_calls++; }

static void dsfx_init(diag_session_fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    mc_config_default(&fx->config);
    mc_keystore_init(&fx->keystore);
    mc_output_hal_t ohal = { .set = NULL, .ctx = NULL };
    mc_output_init(&fx->output, &fx->config.outputs, ohal);

    identity_calib(&fx->calib);
    mc_diag_hal_t dhal = { .read_channel_mv = fake_read_channel_mv, .read_vbat_mv = fake_read_vbat_mv, .ctx = &fx->hal_ctx };
    mc_diag_init(&fx->diag, &fx->config.diagnostics, &fx->calib, dhal);

    fx->app.output = &fx->output;
    fx->app.config = &fx->config;
    fx->app.keystore = &fx->keystore;
    fx->app.diag = &fx->diag;
    fx->app.persist_config = dsfx_persist_config;
    fx->app.persist_diag_calib = dsfx_persist_calib;
    fx->app.app_ctx = fx;
}

/* Enrolls (TOFU) + authenticates a fresh session; mirrors test_lock.c's
 * sfx_auth(). */
static void dsfx_auth(diag_session_fixture_t *fx, mc_session_t *s, recorder_t *rec)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx->keystore, pk, "Phone");

    mc_session_init(s);
    uint8_t begin = MC_OP_AUTH_BEGIN;
    rec_reset(rec);
    mc_session_handle(s, &fx->app, MC_CH_AUTH, &begin, 1, rec_send, rec);
    const rec_frame_t *chal = last_frame(rec, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE);
    assert(chal != NULL);

    uint8_t msg[MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES];
    size_t msg_len = mc_session_build_auth_message(chal->data + 1, msg);
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_sign(sig, msg, msg_len, sk));

    uint8_t resp[1 + MC_CRYPTO_SIG_BYTES];
    resp[0] = MC_OP_AUTH_RESPONSE;
    memcpy(resp + 1, sig, MC_CRYPTO_SIG_BYTES);
    rec_reset(rec);
    mc_session_handle(s, &fx->app, MC_CH_AUTH, resp, sizeof(resp), rec_send, rec);
    assert(mc_session_is_authed(s));
}

static void test_wire_diag_requires_auth(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    mc_session_t unauth;
    mc_session_init(&unauth);
    recorder_t rec = {0};
    uint8_t op = MC_OP_DIAG_GET;
    mc_session_handle(&unauth, &fx.app, MC_CH_COMMAND, &op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_UNAUTHENTICATED);
}

static void test_wire_diag_get(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    dsfx_auth(&fx, &s, &rec);

    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[0] = 777;
    mc_diag_tick(&fx.diag, &fx.output, 0);

    rec_reset(&rec);
    uint8_t op = MC_OP_DIAG_GET;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_DIAG_RESULT);
    assert(r != NULL);
    assert(r->len == 1 + 1 + (size_t)MC_OUTPUT_COUNT * 2 + MC_OUTPUT_COUNT);
    assert(r->data[1] == MC_RESULT_OK);
    uint16_t ch0 = mc_get_u16le(&r->data[2]);
    assert(ch0 == 777);
}

static void test_wire_diag_config_round_trip(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    dsfx_auth(&fx, &s, &rec);

    uint8_t body[MC_OUTPUT_COUNT * 4 + 8];
    size_t pos = 0;
    for (int ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        mc_put_u16le(&body[pos], 111); pos += 2;
        mc_put_u16le(&body[pos], 9999); pos += 2;
    }
    mc_put_u16le(&body[pos], 11900); pos += 2;
    mc_put_u16le(&body[pos], 250); pos += 2;
    mc_put_u16le(&body[pos], 14000); pos += 2;
    mc_put_u16le(&body[pos], 400); pos += 2;
    assert(pos == sizeof(body));

    uint8_t cmd[1 + sizeof(body)];
    cmd[0] = MC_OP_DIAG_SET_CONFIG;
    memcpy(cmd + 1, body, sizeof(body));
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[1] == MC_OP_DIAG_SET_CONFIG && r->data[2] == MC_RESULT_OK);
    assert(fx.persist_config_calls == 1);
    assert(fx.diag.config.lv_cutoff_mv == 11900);
    assert(fx.config.diagnostics.lv_cutoff_mv == 11900); /* persisted copy synced */

    rec_reset(&rec);
    uint8_t getop = MC_OP_DIAG_GET_CONFIG;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &getop, 1, rec_send, &rec);
    const rec_frame_t *cr = last_frame(&rec, MC_CH_COMMAND, MC_OP_DIAG_CONFIG);
    assert(cr != NULL && cr->data[1] == MC_RESULT_OK);
    assert(mc_get_u16le(&cr->data[2]) == 111);

    /* reject inverted thresholds (open_load >= overcurrent) */
    mc_put_u16le(&body[0], 0xFFFF);
    mc_put_u16le(&body[2], 0);
    memcpy(cmd + 1, body, sizeof(body));
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_REJECTED);

    /* short body: bad request */
    rec_reset(&rec);
    uint8_t short_cmd[3] = { MC_OP_DIAG_SET_CONFIG, 0, 0 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, short_cmd, sizeof(short_cmd), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_BAD_REQUEST);
}

static void test_wire_diag_calib_round_trip(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    dsfx_auth(&fx, &s, &rec);

    uint8_t body[16];
    mc_put_f32le(&body[0], 3.5f);
    mc_put_i16le(&body[4], -12);
    mc_put_f32le(&body[6], 1750.0f);
    mc_put_f32le(&body[10], 11.2f);
    mc_put_i16le(&body[14], 5);

    uint8_t cmd[17];
    cmd[0] = MC_OP_DIAG_SET_CALIB;
    memcpy(cmd + 1, body, sizeof(body));
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(fx.persist_diag_calib_calls == 1);
    assert(fx.diag.calib.is_gain == 3.5f);
    assert(fx.diag.calib.is_offset_mv == -12);
    assert(fx.diag.calib.kilis == 1750.0f);

    rec_reset(&rec);
    uint8_t getop = MC_OP_DIAG_GET_CALIB;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &getop, 1, rec_send, &rec);
    const rec_frame_t *cr = last_frame(&rec, MC_CH_COMMAND, MC_OP_DIAG_CALIB);
    assert(cr != NULL && cr->data[1] == MC_RESULT_OK);
    assert(mc_get_f32le(&cr->data[2]) == 3.5f);
    assert(mc_get_i16le(&cr->data[6]) == -12);

    /* short body: bad request, calib unchanged */
    rec_reset(&rec);
    uint8_t short_cmd[5] = { MC_OP_DIAG_SET_CALIB, 0, 0, 0, 0 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, short_cmd, sizeof(short_cmd), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_BAD_REQUEST);
    assert(fx.diag.calib.is_gain == 3.5f);
}

static void test_wire_diag_learn(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    mc_session_t s;
    recorder_t rec = {0};
    dsfx_auth(&fx, &s, &rec);

    assert(mc_output_set(&fx.output, 3, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.hal_ctx.channel_raw_mv[3] = 600;

    rec_reset(&rec);
    uint8_t cmd[2] = { MC_OP_DIAG_LEARN, 3 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(fx.diag.config.channels[3].open_load_ma == 300);
    assert(fx.config.diagnostics.channels[3].open_load_ma == 300);
    assert(fx.persist_config_calls == 1);

    fx.persist_config_calls = 0;
    rec_reset(&rec);
    uint8_t cmd2[2] = { MC_OP_DIAG_LEARN, 7 }; /* channel 7 is off */
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd2, sizeof(cmd2), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_REJECTED);
    assert(fx.persist_config_calls == 0);
}

static void test_wire_diag_ops_degrade_without_diag(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    fx.app.diag = NULL; /* e.g. a host build with no mc_diag attached */
    mc_session_t s;
    recorder_t rec = {0};
    dsfx_auth(&fx, &s, &rec);

    rec_reset(&rec);
    uint8_t op = MC_OP_DIAG_GET;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_DIAG_RESULT);
    assert(r != NULL && r->data[1] == MC_RESULT_REJECTED);

    rec_reset(&rec);
    uint8_t learn[2] = { MC_OP_DIAG_LEARN, 0xFF };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, learn, sizeof(learn), rec_send, &rec);
    const rec_frame_t *lr = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(lr != NULL && lr->data[2] == MC_RESULT_REJECTED);

    rec_reset(&rec);
    uint8_t calib_op = MC_OP_DIAG_GET_CALIB;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &calib_op, 1, rec_send, &rec);
    const rec_frame_t *cr = last_frame(&rec, MC_CH_COMMAND, MC_OP_DIAG_CALIB);
    assert(cr != NULL && cr->data[1] == MC_RESULT_REJECTED);
}

static void test_wire_status_reports_live_diag_state(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    assert(mc_output_set(&fx.output, 0, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    fx.diag.config.channels[0].open_load_ma = 1000; /* force a fault */
    fx.hal_ctx.channel_raw_mv[0] = 10;
    fx.hal_ctx.vbat_raw_mv = 12345;
    mc_diag_tick(&fx.diag, &fx.output, 0);

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t op = MC_OP_STATUS_GET;
    mc_session_handle(&s, &fx.app, MC_CH_STATUS, &op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_STATUS, MC_OP_STATUS);
    assert(r != NULL);

    mc_status_t st;
    assert(mc_status_deserialize(r->data + 1, r->len - 1, &st));
    assert(st.battery_mv == 12345);
    assert(st.output_fault_mask & (1u << 0));
    assert(st.lv_cutoff_active == false);
}

static void test_wire_status_reports_lv_cutoff_active(void)
{
    diag_session_fixture_t fx;
    dsfx_init(&fx);
    /* Channel 9 keeps mc_config_default()'s MC_OUT_FUNC_NONE, which is
     * non-essential (mc_output_function_is_essential()). */
    assert(mc_output_set(&fx.output, 9, true, MC_OUT_SRC_LOCAL) == MC_OUT_OK);
    mc_output_set_lv_cutoff(&fx.output, true);

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t op = MC_OP_STATUS_GET;
    mc_session_handle(&s, &fx.app, MC_CH_STATUS, &op, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_STATUS, MC_OP_STATUS);
    assert(r != NULL);

    mc_status_t st;
    assert(mc_status_deserialize(r->data + 1, r->len - 1, &st));
    assert(st.lv_cutoff_active == true);
    /* commanded_on (intent) still reports ON in output_state_mask even
     * while actually suppressed -- see mc_output_lv_cutoff_active()'s doc
     * comment on why the app needs this separate bit. */
    assert(st.output_state_mask & (1u << 9));
}

int main(void)
{
    test_config_default_and_validate();
    test_calib_default();
    test_sampling_skips_non_energized_channels();
    test_round_robin_one_real_sample_per_tick();
    test_fault_classification();
    test_lv_cutoff_suppressed_channel_never_faulted();
    test_flash_turn_off_phase_never_faulted();
    test_learn_sets_open_load_from_measured_draw();
    test_engine_running_derivation_and_hysteresis();
    test_lv_cutoff_engages_and_recovers_with_hysteresis();
    test_lv_cutoff_gated_on_not_running();
    test_zero_battery_reading_does_not_trigger_cutoff();
    test_apply_config_and_calib();
    test_calib_serialize_roundtrip();
    test_wire_diag_requires_auth();
    test_wire_diag_get();
    test_wire_diag_config_round_trip();
    test_wire_diag_calib_round_trip();
    test_wire_diag_learn();
    test_wire_diag_ops_degrade_without_diag();
    test_wire_status_reports_live_diag_state();
    test_wire_status_reports_lv_cutoff_active();
    return 0;
}
