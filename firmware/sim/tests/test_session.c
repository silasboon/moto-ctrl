#include "mc_session.h"

#include <assert.h>
#include <string.h>

#include "mc_config_json.h"
#include "mc_crypto.h"

/* --- response recorder --- */

typedef struct {
    mc_channel_t ch;
    uint8_t data[MC_CONFIG_JSON_MAX + 16];
    size_t len;
} rec_frame_t;

typedef struct {
    rec_frame_t frames[64];
    int count;
} recorder_t;

static void rec_send(void *io, mc_channel_t ch, const uint8_t *data, size_t len)
{
    recorder_t *r = (recorder_t *)io;
    assert(r->count < 64);
    assert(len <= sizeof(r->frames[0].data));
    r->frames[r->count].ch = ch;
    memcpy(r->frames[r->count].data, data, len);
    r->frames[r->count].len = len;
    r->count++;
}

static void rec_reset(recorder_t *r)
{
    r->count = 0;
}

/* Returns the last recorded frame on channel `ch` whose opcode == `opcode`, or NULL. */
static const rec_frame_t *last_frame(const recorder_t *r, mc_channel_t ch, uint8_t opcode)
{
    for (int i = r->count - 1; i >= 0; i--) {
        if (r->frames[i].ch == ch && r->frames[i].len >= 1 && r->frames[i].data[0] == opcode) {
            return &r->frames[i];
        }
    }
    return NULL;
}

/* --- Fakes: OTA flash HAL + event log HAL --- */

#define FAKE_FLASH_MAX 65536

typedef struct {
    uint8_t buf[FAKE_FLASH_MAX];
    uint32_t len;
    int begin_calls, write_calls, finalize_calls, abort_calls, reboot_calls;
} fake_ota_flash_t;

static bool fake_flash_begin(uint32_t image_size, void *ctx)
{
    fake_ota_flash_t *f = (fake_ota_flash_t *)ctx;
    f->begin_calls++;
    f->len = 0;
    return image_size <= FAKE_FLASH_MAX;
}
static bool fake_flash_write(uint32_t offset, const uint8_t *data, size_t len, void *ctx)
{
    fake_ota_flash_t *f = (fake_ota_flash_t *)ctx;
    f->write_calls++;
    if ((uint64_t)offset + (uint64_t)len > FAKE_FLASH_MAX) {
        return false;
    }
    memcpy(f->buf + offset, data, len);
    if (offset + len > f->len) {
        f->len = offset + (uint32_t)len;
    }
    return true;
}
static bool fake_flash_finalize(void *ctx) { ((fake_ota_flash_t *)ctx)->finalize_calls++; return true; }
static void fake_flash_abort(void *ctx) { ((fake_ota_flash_t *)ctx)->abort_calls++; }
static void fake_flash_reboot(void *ctx) { ((fake_ota_flash_t *)ctx)->reboot_calls++; }

typedef struct {
    mc_event_record_t records[MC_EVENT_LOG_SLOT_COUNT];
    bool present[MC_EVENT_LOG_SLOT_COUNT];
    uint32_t last_seq;
} fake_evtlog_store_t;

static bool fake_evt_read(uint16_t slot, mc_event_record_t *out, void *ctx)
{
    fake_evtlog_store_t *e = (fake_evtlog_store_t *)ctx;
    if (slot >= MC_EVENT_LOG_SLOT_COUNT || !e->present[slot]) {
        return false;
    }
    *out = e->records[slot];
    return true;
}
static bool fake_evt_write(uint16_t slot, const mc_event_record_t *rec, void *ctx)
{
    fake_evtlog_store_t *e = (fake_evtlog_store_t *)ctx;
    if (slot >= MC_EVENT_LOG_SLOT_COUNT) {
        return false;
    }
    e->records[slot] = *rec;
    e->present[slot] = true;
    return true;
}
static uint32_t fake_evt_get_last_seq(void *ctx) { return ((fake_evtlog_store_t *)ctx)->last_seq; }
static bool fake_evt_set_last_seq(uint32_t seq, void *ctx)
{
    ((fake_evtlog_store_t *)ctx)->last_seq = seq;
    return true;
}

/* --- app fixture --- */

typedef struct {
    mc_output_engine_t output;
    mc_config_t config;
    mc_keystore_t keystore;
    mc_lock_t lock;

    mc_ota_t ota;
    fake_ota_flash_t flash;
    uint8_t ota_pubkey[MC_CRYPTO_PUBKEY_BYTES];
    uint8_t ota_secret[MC_CRYPTO_SECRETKEY_BYTES];

    mc_event_log_t event_log;
    fake_evtlog_store_t evtlog_store;

    mc_app_t app;
    int persist_config_calls;
    int persist_keystore_calls;
    int persist_lock_calls;
    int log_event_calls;
    uint8_t last_log_type, last_log_arg0, last_log_arg1;
} fixture_t;

static void fx_persist_config(void *ctx) { ((fixture_t *)ctx)->persist_config_calls++; }
static void fx_persist_keystore(void *ctx) { ((fixture_t *)ctx)->persist_keystore_calls++; }
/* Mirrors main.c's/sim's real persist_lock_cb, which clears the dirty flag
 * after persisting -- without this, mc_lock_is_dirty() would stay true
 * forever and every subsequent idempotent command would re-persist/re-log. */
static void fx_persist_lock(void *ctx)
{
    fixture_t *fx = (fixture_t *)ctx;
    fx->persist_lock_calls++;
    mc_lock_clear_dirty(&fx->lock);
}
/* Spies on every call AND forwards to the real mc_event_log_append(), like
 * main.c's/sim's real log_event callback does -- otherwise
 * mc_event_log_clear()-then-log-the-wipe sequences (MC_OP_TRANSFER_OWNERSHIP)
 * would silently diverge from production wiring: mc_session.c calls
 * mc_event_log_clear() on app->event_log directly, but the "log the wipe"
 * step goes through this callback, which must land in the same log. */
static void fx_log_event(void *ctx, uint8_t type, uint8_t arg0, uint8_t arg1)
{
    fixture_t *fx = (fixture_t *)ctx;
    fx->log_event_calls++;
    fx->last_log_type = type;
    fx->last_log_arg0 = arg0;
    fx->last_log_arg1 = arg1;
    mc_event_log_append(&fx->event_log, (mc_event_type_t)type, arg0, arg1, 0);
}

static void fixture_init(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    mc_config_default(&fx->config);
    mc_keystore_init(&fx->keystore);
    mc_output_hal_t hal = { .set = NULL, .ctx = NULL };
    mc_output_init(&fx->output, &fx->config.outputs, hal);

    mc_crypto_keypair(fx->ota_pubkey, fx->ota_secret);
    mc_ota_hal_t ota_hal = {
        .flash_begin = fake_flash_begin,
        .flash_write = fake_flash_write,
        .flash_finalize = fake_flash_finalize,
        .flash_abort = fake_flash_abort,
        .reboot = fake_flash_reboot,
        .ctx = &fx->flash,
    };
    mc_ota_init(&fx->ota, ota_hal, fx->ota_pubkey);

    mc_event_log_hal_t evt_hal = {
        .read_slot = fake_evt_read,
        .write_slot = fake_evt_write,
        .get_last_seq = fake_evt_get_last_seq,
        .set_last_seq = fake_evt_set_last_seq,
        .ctx = &fx->evtlog_store,
    };
    mc_event_log_init(&fx->event_log, evt_hal);

    /* app->lock stays NULL unless a test opts in via fx_enable_lock() below
     * (LOCK/UNLOCK ops on a NULL lock just report REJECTED, matching every
     * other test in this file that doesn't care about lock behavior). */
    fx->app.output = &fx->output;
    fx->app.config = &fx->config;
    fx->app.keystore = &fx->keystore;
    fx->app.ota = &fx->ota;
    fx->app.event_log = &fx->event_log;
    fx->app.fill_status = NULL;
    fx->app.persist_config = fx_persist_config;
    fx->app.persist_keystore = fx_persist_keystore;
    fx->app.persist_lock = fx_persist_lock;
    fx->app.log_event = fx_log_event;
    fx->app.app_ctx = fx;
}

/* Configures a minimal PHONE-method immobilizer (ignition channel 5,
 * cheat-code {1,2,3,4} set so MC_LOCK_CFG_ENABLE_REQUIRES_CHEATCODE is
 * satisfied) and points app->lock at it. Starts UNLOCKED. Mirrors
 * test_lock.c's fx_enable_with_cheatcode(). */
static void fx_enable_lock(fixture_t *fx)
{
    fx->config.outputs.channels[5].is_ignition = true;
    fx->config.outputs.channels[5].essential = true;
    fx->output.config = fx->config.outputs;

    mc_lock_config_t cfg;
    mc_lock_config_default(&cfg);
    uint8_t code[4] = { 1, 2, 3, 4 };
    mc_lock_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.config = cfg;
    assert(mc_lock_set_cheatcode(&tmp, code, 4, 0));
    tmp.config.immobilizer_enabled = true;
    tmp.config.methods_mask = MC_LOCK_METHOD_PHONE;
    mc_lock_init(&fx->lock, &tmp.config, false, &fx->output, 0);
    assert(fx->lock.state == MC_LOCK_ST_UNLOCKED);

    fx->app.lock = &fx->lock;
}

/* Runs the full challenge-response against a session using key (pk already
 * enrolled) `sk`. Asserts success and returns the authenticated slot. */
static int do_auth(mc_session_t *s, mc_app_t *app, const uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES], recorder_t *rec)
{
    rec_reset(rec);
    uint8_t begin = MC_OP_AUTH_BEGIN;
    mc_session_handle(s, app, MC_CH_AUTH, &begin, 1, rec_send, rec);

    const rec_frame_t *chal = last_frame(rec, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE);
    assert(chal != NULL);
    assert(chal->len == 1 + MC_CRYPTO_NONCE_BYTES);
    const uint8_t *nonce = chal->data + 1;

    uint8_t msg[MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES];
    size_t msg_len = mc_session_build_auth_message(nonce, msg);
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_sign(sig, msg, msg_len, sk));

    uint8_t resp[1 + MC_CRYPTO_SIG_BYTES];
    resp[0] = MC_OP_AUTH_RESPONSE;
    memcpy(resp + 1, sig, MC_CRYPTO_SIG_BYTES);
    rec_reset(rec);
    mc_session_handle(s, app, MC_CH_AUTH, resp, sizeof(resp), rec_send, rec);

    const rec_frame_t *result = last_frame(rec, MC_CH_AUTH, MC_OP_AUTH_RESULT);
    assert(result != NULL);
    assert(result->len == 3);
    assert(result->data[1] == MC_RESULT_OK);
    assert(mc_session_is_authed(s));
    return result->data[2];
}

static void test_status_readable_without_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t get = MC_OP_STATUS_GET;
    mc_session_handle(&s, &fx.app, MC_CH_STATUS, &get, 1, rec_send, &rec);

    const rec_frame_t *st = last_frame(&rec, MC_CH_STATUS, MC_OP_STATUS);
    assert(st != NULL);
    assert(st->len == 1 + MC_STATUS_WIRE_LEN);
    /* firmware version present */
    assert(st->data[1] == MC_FW_VERSION_MAJOR);
}

static void test_control_rejected_before_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].essential = true;
    fx.output.config = fx.config.outputs;

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t cmd[3] = { MC_OP_SET_OUTPUT, 0, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);

    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL);
    assert(res->data[2] == MC_RESULT_UNAUTHENTICATED);
    assert(mc_output_get_state(&fx.output, 0) == false);
}

static void test_enroll_tofu_then_auth_then_control(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].essential = true;
    fx.output.config = fx.config.outputs;

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    /* First-key enrollment allowed on an empty keystore (TOFU). */
    uint8_t enroll[1 + MC_CRYPTO_PUBKEY_BYTES + 5];
    enroll[0] = MC_OP_ENROLL;
    memcpy(enroll + 1, pk, MC_CRYPTO_PUBKEY_BYTES);
    memcpy(enroll + 1 + MC_CRYPTO_PUBKEY_BYTES, "Phone", 5);
    mc_session_handle(&s, &fx.app, MC_CH_AUTH, enroll, sizeof(enroll), rec_send, &rec);
    const rec_frame_t *er = last_frame(&rec, MC_CH_AUTH, MC_OP_ENROLL_RESULT);
    assert(er != NULL && er->data[1] == MC_RESULT_OK);
    assert(fx.persist_keystore_calls == 1);
    assert(mc_keystore_count(&fx.keystore) == 1);

    /* Enrollment does NOT authenticate the session. */
    assert(!mc_session_is_authed(&s));

    do_auth(&s, &fx.app, sk, &rec);

    /* Now control works and drives the output engine. */
    rec_reset(&rec);
    uint8_t cmd[3] = { MC_OP_SET_OUTPUT, 0, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL && res->data[2] == MC_RESULT_OK);
    assert(mc_output_get_state(&fx.output, 0) == true);
    assert(fx.persist_config_calls == 1);
}

static void test_starter_rejected_over_ble_even_when_authed(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[5].is_starter = true;
    fx.output.config = fx.config.outputs;

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t cmd[3] = { MC_OP_SET_OUTPUT, 5, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL);
    assert(res->data[2] == MC_RESULT_REJECTED); /* starter never triggerable from app */
    assert(mc_output_get_state(&fx.output, 5) == false);
}

/* --- MC_OP_HAZARD_PRESS wire dispatch --- */

static void test_hazard_press_rejected_without_turn_channels(void)
{
    fixture_t fx;
    fixture_init(&fx); /* no TURN_L/TURN_R channel configured */

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t cmd[1] = { MC_OP_HAZARD_PRESS };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL);
    assert(res->data[2] == MC_RESULT_REJECTED);
}

static void test_hazard_press_rejected_before_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].indicator = MC_INDICATOR_LEFT; fx.config.outputs.channels[0].hazard_member = true;
    fx.config.outputs.channels[1].indicator = MC_INDICATOR_RIGHT; fx.config.outputs.channels[1].hazard_member = true;
    fx.output.config = fx.config.outputs;

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t cmd[1] = { MC_OP_HAZARD_PRESS };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL);
    assert(res->data[2] == MC_RESULT_UNAUTHENTICATED);
    assert(mc_output_get_state(&fx.output, 0) == false);
    assert(mc_output_get_state(&fx.output, 1) == false);
}

static void test_hazard_press_toggles_both_when_authed(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].indicator = MC_INDICATOR_LEFT; fx.config.outputs.channels[0].hazard_member = true;
    fx.config.outputs.channels[1].indicator = MC_INDICATOR_RIGHT; fx.config.outputs.channels[1].hazard_member = true;
    fx.output.config = fx.config.outputs;

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t cmd[1] = { MC_OP_HAZARD_PRESS };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL && res->data[2] == MC_RESULT_OK);
    assert(mc_output_get_state(&fx.output, 0) == true);
    assert(mc_output_get_state(&fx.output, 1) == true);
    assert(fx.persist_config_calls == 1);

    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL && res->data[2] == MC_RESULT_OK);
    assert(mc_output_get_state(&fx.output, 0) == false); /* both on -> toggles both off */
    assert(mc_output_get_state(&fx.output, 1) == false);
}

static void test_replayed_signature_fails_on_new_session(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    /* Session 1: capture its challenge nonce + the signature we send. */
    mc_session_t s1;
    mc_session_init(&s1);
    recorder_t rec = {0};
    uint8_t begin = MC_OP_AUTH_BEGIN;
    mc_session_handle(&s1, &fx.app, MC_CH_AUTH, &begin, 1, rec_send, &rec);
    const rec_frame_t *chal1 = last_frame(&rec, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE);
    uint8_t nonce1[MC_CRYPTO_NONCE_BYTES];
    memcpy(nonce1, chal1->data + 1, MC_CRYPTO_NONCE_BYTES);

    uint8_t msg1[MC_AUTH_CONTEXT_LEN + MC_CRYPTO_NONCE_BYTES];
    size_t msg1_len = mc_session_build_auth_message(nonce1, msg1);
    uint8_t sig1[MC_CRYPTO_SIG_BYTES];
    mc_crypto_sign(sig1, msg1, msg1_len, sk);

    /* Session 2: fresh challenge -> different nonce. */
    mc_session_t s2;
    mc_session_init(&s2);
    rec_reset(&rec);
    mc_session_handle(&s2, &fx.app, MC_CH_AUTH, &begin, 1, rec_send, &rec);
    const rec_frame_t *chal2 = last_frame(&rec, MC_CH_AUTH, MC_OP_AUTH_CHALLENGE);
    assert(memcmp(nonce1, chal2->data + 1, MC_CRYPTO_NONCE_BYTES) != 0);

    /* Replaying session 1's signature into session 2 must fail. */
    uint8_t resp[1 + MC_CRYPTO_SIG_BYTES];
    resp[0] = MC_OP_AUTH_RESPONSE;
    memcpy(resp + 1, sig1, MC_CRYPTO_SIG_BYTES);
    rec_reset(&rec);
    mc_session_handle(&s2, &fx.app, MC_CH_AUTH, resp, sizeof(resp), rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_AUTH, MC_OP_AUTH_RESULT);
    assert(res != NULL);
    assert(res->data[1] == MC_RESULT_REJECTED);
    assert(!mc_session_is_authed(&s2));
}

static void test_enroll_denied_when_unauth_and_store_nonempty(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk0[MC_CRYPTO_PUBKEY_BYTES], sk0[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk0, sk0);
    mc_keystore_add(&fx.keystore, pk0, "Owner");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t pk1[MC_CRYPTO_PUBKEY_BYTES], sk1[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk1, sk1);
    uint8_t enroll[1 + MC_CRYPTO_PUBKEY_BYTES];
    enroll[0] = MC_OP_ENROLL;
    memcpy(enroll + 1, pk1, MC_CRYPTO_PUBKEY_BYTES);
    mc_session_handle(&s, &fx.app, MC_CH_AUTH, enroll, sizeof(enroll), rec_send, &rec);

    const rec_frame_t *er = last_frame(&rec, MC_CH_AUTH, MC_OP_ENROLL_RESULT);
    assert(er != NULL && er->data[1] == MC_RESULT_ENROLL_DENIED);
    assert(mc_keystore_count(&fx.keystore) == 1);

    /* But an authenticated owner CAN enroll a second key. */
    do_auth(&s, &fx.app, sk0, &rec);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_AUTH, enroll, sizeof(enroll), rec_send, &rec);
    er = last_frame(&rec, MC_CH_AUTH, MC_OP_ENROLL_RESULT);
    assert(er != NULL && er->data[1] == MC_RESULT_OK);
    assert(mc_keystore_count(&fx.keystore) == 2);
}

static void test_key_list_and_revoke(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    int slot = mc_keystore_add(&fx.keystore, pk, "Owner");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t list = MC_OP_KEY_LIST;
    mc_session_handle(&s, &fx.app, MC_CH_AUTH, &list, 1, rec_send, &rec);
    const rec_frame_t *lr = last_frame(&rec, MC_CH_AUTH, MC_OP_KEY_LIST_RESULT);
    assert(lr != NULL);
    assert(lr->data[1] == 1); /* count */

    rec_reset(&rec);
    uint8_t revoke[2] = { MC_OP_KEY_REVOKE, (uint8_t)slot };
    mc_session_handle(&s, &fx.app, MC_CH_AUTH, revoke, sizeof(revoke), rec_send, &rec);
    const rec_frame_t *rr = last_frame(&rec, MC_CH_AUTH, MC_OP_KEY_REVOKE_RESULT);
    assert(rr != NULL && rr->data[1] == MC_RESULT_OK);
    assert(mc_keystore_count(&fx.keystore) == 0);
}

/* Reassemble a chunked CONFIG_READ into a single buffer. */
static size_t collect_config_read(const recorder_t *rec, uint8_t *out, size_t out_cap)
{
    size_t total = 0;
    for (int i = 0; i < rec->count; i++) {
        const rec_frame_t *f = &rec->frames[i];
        if (f->ch != MC_CH_CONFIG || f->len < 5 || f->data[0] != MC_OP_CONFIG_CHUNK) {
            continue;
        }
        uint16_t offset = mc_get_u16le(f->data + 1);
        uint16_t t = mc_get_u16le(f->data + 3);
        total = t;
        size_t n = f->len - 5;
        assert((size_t)offset + n <= out_cap);
        memcpy(out + offset, f->data + 5, n);
    }
    return total;
}

static void test_config_read_write_roundtrip_preserves_output_state(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].essential = true;
    fx.output.config = fx.config.outputs;

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    /* Turn channel 0 ON as a live state before importing new config. */
    rec_reset(&rec);
    uint8_t cmd[3] = { MC_OP_SET_OUTPUT, 0, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    assert(mc_output_get_state(&fx.output, 0) == true);

    /* Read current config back. */
    rec_reset(&rec);
    uint8_t read = MC_OP_CONFIG_READ;
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, &read, 1, rec_send, &rec);
    uint8_t jsonbuf[MC_CONFIG_JSON_MAX];
    size_t json_len = collect_config_read(&rec, jsonbuf, sizeof(jsonbuf));
    assert(json_len > 0);
    mc_config_t parsed;
    assert(mc_config_from_json((const char *)jsonbuf, json_len, &parsed) == MC_CONFIG_OK);
    assert(parsed.outputs.channels[0].essential);

    /* Build a NEW config (rename channel 0, add a horn) and write it back
     * chunked. commanded_on in the JSON is false, but the live ON state
     * must be preserved after commit. */
    mc_config_t newcfg;
    mc_config_default(&newcfg);
    newcfg.outputs.channels[0].essential = true;
    strcpy(newcfg.outputs.channels[0].name, "Low Beam");
    newcfg.outputs.channels[1].behaviour = MC_OUT_BEHAVIOUR_TOGGLE;
    char *newjson = mc_config_to_json(&newcfg);
    size_t newlen = strlen(newjson);

    uint8_t begin[3];
    begin[0] = MC_OP_CONFIG_WRITE_BEGIN;
    mc_put_u16le(begin + 1, (uint16_t)newlen);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, begin, sizeof(begin), rec_send, &rec);

    /* Two chunks to exercise reassembly. */
    size_t half = newlen / 2;
    uint8_t chunk[3 + MC_CONFIG_JSON_MAX];
    chunk[0] = MC_OP_CONFIG_WRITE_CHUNK;
    mc_put_u16le(chunk + 1, 0);
    memcpy(chunk + 3, newjson, half);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, chunk, 3 + half, rec_send, &rec);
    mc_put_u16le(chunk + 1, (uint16_t)half);
    memcpy(chunk + 3, newjson + half, newlen - half);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, chunk, 3 + (newlen - half), rec_send, &rec);

    rec_reset(&rec);
    uint8_t commit = MC_OP_CONFIG_WRITE_COMMIT;
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, &commit, 1, rec_send, &rec);
    const rec_frame_t *wr = last_frame(&rec, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT);
    assert(wr != NULL && wr->data[1] == MC_RESULT_OK);

    /* New config applied... */
    assert((fx.config.outputs.channels[1].behaviour == MC_OUT_BEHAVIOUR_TOGGLE));
    assert(strcmp(fx.config.outputs.channels[0].name, "Low Beam") == 0);
    /* ...but the live output state was preserved (not toggled by import). */
    assert(mc_output_get_state(&fx.output, 0) == true);
    assert(fx.config.outputs.channels[0].commanded_on == true);

    mc_config_json_free(newjson);
}

static void test_config_write_preserves_imported_mode_not_just_on_off(void)
{
    /* Regression coverage: config_commit() must not force every channel's
     * `mode` to mirror the live on/off state, which would silently discard
     * an imported MC_OUT_MODE_FLASH_TURN/PWM/etc. choice on every single
     * commit. mode must come through from the imported JSON as-is -- only
     * commanded_on is preserved from live state (AGENTS.md #1: import must
     * never toggle outputs). */
    fixture_t fx;
    fixture_init(&fx);
    fx.config.outputs.channels[0].indicator = MC_INDICATOR_LEFT; fx.config.outputs.channels[0].hazard_member = true;
    fx.output.config = fx.config.outputs;

    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    /* Turn channel 0 ON live before importing. */
    rec_reset(&rec);
    uint8_t cmd[3] = { MC_OP_SET_OUTPUT, 0, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, cmd, sizeof(cmd), rec_send, &rec);
    assert(mc_output_get_state(&fx.output, 0) == true);

    mc_config_t newcfg;
    mc_config_default(&newcfg);
    newcfg.outputs.channels[0].indicator = MC_INDICATOR_LEFT; newcfg.outputs.channels[0].hazard_member = true;
    newcfg.outputs.channels[0].behaviour = MC_OUT_BEHAVIOUR_BLINK; /* the point of this import */
    char *newjson = mc_config_to_json(&newcfg);
    size_t newlen = strlen(newjson);

    uint8_t begin[3];
    begin[0] = MC_OP_CONFIG_WRITE_BEGIN;
    mc_put_u16le(begin + 1, (uint16_t)newlen);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, begin, sizeof(begin), rec_send, &rec);

    uint8_t chunk[3 + MC_CONFIG_JSON_MAX];
    chunk[0] = MC_OP_CONFIG_WRITE_CHUNK;
    mc_put_u16le(chunk + 1, 0);
    memcpy(chunk + 3, newjson, newlen);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, chunk, 3 + newlen, rec_send, &rec);

    rec_reset(&rec);
    uint8_t commit = MC_OP_CONFIG_WRITE_COMMIT;
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, &commit, 1, rec_send, &rec);
    const rec_frame_t *wr = last_frame(&rec, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT);
    assert(wr != NULL && wr->data[1] == MC_RESULT_OK);

    assert(fx.config.outputs.channels[0].behaviour == MC_OUT_BEHAVIOUR_BLINK); /* imported behaviour survives */
    assert(mc_output_get_state(&fx.output, 0) == true);                  /* live on/off state still preserved */

    mc_config_json_free(newjson);
}

static void test_config_write_rejects_invalid_config(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    /* Two ignition channels -> validation must reject on commit. */
    mc_config_t bad;
    mc_config_default(&bad);
    bad.outputs.channels[0].is_ignition = true; bad.outputs.channels[0].essential = true;
    bad.outputs.channels[1].is_ignition = true; bad.outputs.channels[1].essential = true;
    char *json = mc_config_to_json(&bad);
    size_t len = strlen(json);

    uint8_t begin[3];
    begin[0] = MC_OP_CONFIG_WRITE_BEGIN;
    mc_put_u16le(begin + 1, (uint16_t)len);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, begin, sizeof(begin), rec_send, &rec);

    uint8_t chunk[3 + MC_CONFIG_JSON_MAX];
    chunk[0] = MC_OP_CONFIG_WRITE_CHUNK;
    mc_put_u16le(chunk + 1, 0);
    memcpy(chunk + 3, json, len);
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, chunk, 3 + len, rec_send, &rec);

    rec_reset(&rec);
    uint8_t commit = MC_OP_CONFIG_WRITE_COMMIT;
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, &commit, 1, rec_send, &rec);
    const rec_frame_t *wr = last_frame(&rec, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT);
    assert(wr != NULL && wr->data[1] == MC_RESULT_REJECTED);

    mc_config_json_free(json);
}

static void test_config_rejected_before_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t read = MC_OP_CONFIG_READ;
    mc_session_handle(&s, &fx.app, MC_CH_CONFIG, &read, 1, rec_send, &rec);
    const rec_frame_t *wr = last_frame(&rec, MC_CH_CONFIG, MC_OP_CONFIG_WRITE_RESULT);
    assert(wr != NULL && wr->data[1] == MC_RESULT_UNAUTHENTICATED);
}

/* --- OTA wire dispatch --- */

static void build_ota_begin_payload(const fixture_t *fx, const uint8_t *image, size_t image_len,
                                    uint8_t out[4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES])
{
    mc_put_u32le(out, (uint32_t)image_len);
    uint8_t sha[MC_CRYPTO_HASH_BYTES];
    assert(mc_crypto_hash_sha512(image, image_len, sha));
    memcpy(out + 4, sha, MC_CRYPTO_HASH_BYTES);
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_sign(sig, sha, MC_CRYPTO_HASH_BYTES, fx->ota_secret));
    memcpy(out + 4 + MC_CRYPTO_HASH_BYTES, sig, MC_CRYPTO_SIG_BYTES);
}

static void test_ota_rejected_before_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t begin[1 + 4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES];
    begin[0] = MC_OP_OTA_BEGIN;
    build_ota_begin_payload(&fx, (const uint8_t *)"abc", 3, begin + 1);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, begin, sizeof(begin), rec_send, &rec);

    const rec_frame_t *r = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r != NULL && r->data[1] == MC_RESULT_UNAUTHENTICATED);
    assert(fx.flash.begin_calls == 0);
}

static void test_ota_full_transfer_via_session(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    uint8_t image[300];
    for (size_t i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)(i * 3 + 7);
    }

    uint8_t begin[1 + 4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES];
    begin[0] = MC_OP_OTA_BEGIN;
    build_ota_begin_payload(&fx, image, sizeof(image), begin + 1);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, begin, sizeof(begin), rec_send, &rec);
    const rec_frame_t *r1 = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r1 != NULL && r1->data[1] == MC_RESULT_OK);
    assert(fx.flash.begin_calls == 1);
    assert(fx.log_event_calls == 1 && fx.last_log_type == MC_EVT_OTA_BEGIN);

    /* Two in-order chunks. */
    size_t half = sizeof(image) / 2;
    uint8_t chunk[1 + 4 + 256];
    chunk[0] = MC_OP_OTA_CHUNK;
    mc_put_u32le(chunk + 1, 0);
    memcpy(chunk + 5, image, half);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, chunk, 5 + half, rec_send, &rec);
    assert(last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT)->data[1] == MC_RESULT_OK);

    chunk[0] = MC_OP_OTA_CHUNK;
    mc_put_u32le(chunk + 1, (uint32_t)half);
    memcpy(chunk + 5, image + half, sizeof(image) - half);
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, chunk, 5 + (sizeof(image) - half), rec_send, &rec);
    assert(last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT)->data[1] == MC_RESULT_OK);
    assert(fx.flash.write_calls == 2);

    /* Status mid-flight reflects bytes received so far. */
    rec_reset(&rec);
    uint8_t status_op = MC_OP_OTA_STATUS;
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &status_op, 1, rec_send, &rec);
    const rec_frame_t *sr = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_STATUS_RESULT);
    assert(sr != NULL && sr->len == 1 + 10);
    assert(sr->data[1] == MC_RESULT_OK);
    assert(sr->data[2] == MC_OTA_RECEIVING);
    assert(mc_get_u32le(&sr->data[3]) == sizeof(image));
    assert(mc_get_u32le(&sr->data[7]) == sizeof(image));

    rec_reset(&rec);
    uint8_t commit = MC_OP_OTA_COMMIT;
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &commit, 1, rec_send, &rec);
    const rec_frame_t *r4 = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r4 != NULL && r4->data[1] == MC_RESULT_OK);
    assert(fx.flash.finalize_calls == 1);
    assert(fx.flash.len == sizeof(image));
    assert(memcmp(fx.flash.buf, image, sizeof(image)) == 0);
    assert(fx.log_event_calls == 2 && fx.last_log_type == MC_EVT_OTA_SUCCESS);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);

    /* Reboot: safe by default (engine not running, no lv cutoff). */
    rec_reset(&rec);
    uint8_t reboot = MC_OP_OTA_REBOOT;
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &reboot, 1, rec_send, &rec);
    const rec_frame_t *r5 = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r5 != NULL && r5->data[1] == MC_RESULT_OK);
    assert(fx.flash.reboot_calls == 1);
}

static void test_ota_begin_rejects_bad_signature(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    uint8_t image[64] = {0};
    uint8_t begin[1 + 4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES];
    begin[0] = MC_OP_OTA_BEGIN;
    build_ota_begin_payload(&fx, image, sizeof(image), begin + 1);
    begin[1 + 4] ^= 0xFF; /* corrupt the declared sha512: signature no longer verifies */

    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, begin, sizeof(begin), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r != NULL && r->data[1] == MC_RESULT_REJECTED);
    assert(fx.flash.begin_calls == 0); /* rejected before any flash write */
    assert(fx.log_event_calls == 1 && fx.last_log_type == MC_EVT_OTA_FAILURE);
}

static void test_ota_reboot_blocked_while_engine_running(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    uint8_t image[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t begin[1 + 4 + MC_CRYPTO_HASH_BYTES + MC_CRYPTO_SIG_BYTES];
    begin[0] = MC_OP_OTA_BEGIN;
    build_ota_begin_payload(&fx, image, sizeof(image), begin + 1);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, begin, sizeof(begin), rec_send, &rec);

    uint8_t chunk[1 + 4 + 16];
    chunk[0] = MC_OP_OTA_CHUNK;
    mc_put_u32le(chunk + 1, 0);
    memcpy(chunk + 5, image, sizeof(image));
    mc_session_handle(&s, &fx.app, MC_CH_OTA, chunk, sizeof(chunk), rec_send, &rec);

    uint8_t commit = MC_OP_OTA_COMMIT;
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &commit, 1, rec_send, &rec);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);

    /* Engine now "running": reboot must be refused, COMMITTED left intact
     * so the client can retry later (nothing about the transfer is lost). */
    fx.output.engine_running = true;
    rec_reset(&rec);
    uint8_t reboot = MC_OP_OTA_REBOOT;
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &reboot, 1, rec_send, &rec);
    const rec_frame_t *r1 = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r1 != NULL && r1->data[1] == MC_RESULT_REJECTED);
    assert(fx.flash.reboot_calls == 0);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);

    /* Engine stops: retry succeeds. */
    fx.output.engine_running = false;
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_OTA, &reboot, 1, rec_send, &rec);
    const rec_frame_t *r2 = last_frame(&rec, MC_CH_OTA, MC_OP_OTA_RESULT);
    assert(r2 != NULL && r2->data[1] == MC_RESULT_OK);
    assert(fx.flash.reboot_calls == 1);
}

/* --- Event log wire dispatch --- */

static void test_event_log_get_paginated(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    /* 15 records > MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS (10) -- forces 2 chunks. */
    for (int i = 0; i < 15; i++) {
        mc_event_log_append(&fx.event_log, MC_EVT_LOCK_ENGAGED, (uint8_t)i, 0, (uint32_t)(1000 + i));
    }

    rec_reset(&rec);
    uint8_t req[1 + 4];
    req[0] = MC_OP_EVENT_LOG_GET;
    mc_put_u32le(req + 1, 0);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, req, sizeof(req), rec_send, &rec);

    int chunk_count = 0;
    uint32_t reconstructed_seqs[32];
    size_t reconstructed_count = 0;
    for (int i = 0; i < rec.count; i++) {
        const rec_frame_t *f = &rec.frames[i];
        if (f->ch != MC_CH_COMMAND || f->len < 6 || f->data[0] != MC_OP_EVENT_LOG_CHUNK) {
            continue;
        }
        chunk_count++;
        uint16_t total = mc_get_u16le(f->data + 3);
        assert(total == 15);
        uint8_t count = f->data[5];
        size_t pos = 6;
        for (uint8_t ri = 0; ri < count; ri++) {
            uint32_t seq = mc_get_u32le(f->data + pos); pos += 4;
            uint32_t uptime_ms = mc_get_u32le(f->data + pos); pos += 4;
            uint8_t type = f->data[pos++];
            uint8_t arg0 = f->data[pos++];
            pos += 2; /* arg1, reserved */
            assert(type == MC_EVT_LOCK_ENGAGED);
            assert(arg0 == (uint8_t)(seq - 1));
            assert(uptime_ms == 1000u + (seq - 1));
            reconstructed_seqs[reconstructed_count++] = seq;
        }
    }
    assert(chunk_count == 2);
    assert(reconstructed_count == 15);
    for (size_t i = 0; i < 15; i++) {
        assert(reconstructed_seqs[i] == i + 1); /* oldest-first */
    }
}

static void test_event_log_get_rejected_when_unavailable(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.app.event_log = NULL; /* simulates a host with no event log attached */
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t req[1 + 4] = { MC_OP_EVENT_LOG_GET, 0, 0, 0, 0 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, req, sizeof(req), rec_send, &rec);
    const rec_frame_t *f = last_frame(&rec, MC_CH_COMMAND, MC_OP_EVENT_LOG_CHUNK);
    assert(f != NULL && f->len == 6);
    assert(f->data[5] == 0); /* count == 0 */
}

/* --- Security-relevant events logged at their mc_session.c call sites --- */

static void test_lock_engage_logs_event(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx_enable_lock(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    rec_reset(&rec);
    uint8_t cmd = MC_OP_LOCK;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &cmd, 1, rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL && res->data[2] == MC_RESULT_OK);
    assert(fx.lock.state == MC_LOCK_ST_LOCKED);
    assert(fx.log_event_calls == 1 && fx.last_log_type == MC_EVT_LOCK_ENGAGED);

    /* Idempotent re-lock (already LOCKED) must not log again -- gated on
     * mc_lock_is_dirty(), same as the persist_lock call right next to it. */
    rec_reset(&rec);
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &cmd, 1, rec_send, &rec);
    assert(fx.log_event_calls == 1);
}

static void test_transfer_ownership_logs_event_and_wipes_log(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx_enable_lock(&fx);
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    mc_keystore_add(&fx.keystore, pk, "Phone");

    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    do_auth(&s, &fx.app, sk, &rec);

    /* Some history before the transfer. */
    mc_event_log_append(&fx.event_log, MC_EVT_KEY_ENROLLED, 0, 0, 500);
    mc_event_log_append(&fx.event_log, MC_EVT_KEY_ENROLLED, 1, 0, 600);
    assert(mc_event_log_count_since(&fx.event_log, 0) == 2);

    rec_reset(&rec);
    uint8_t cmd = MC_OP_TRANSFER_OWNERSHIP;
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &cmd, 1, rec_send, &rec);
    const rec_frame_t *res = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(res != NULL && res->data[2] == MC_RESULT_OK);

    /* Old history is gone; exactly one record survives: the transfer itself
     * (docs/PROTOCOL.md §15's documented wipe-then-log-the-wipe doctrine). */
    assert(mc_event_log_count_since(&fx.event_log, 0) == 1);
    assert(fx.last_log_type == MC_EVT_OWNERSHIP_TRANSFERRED);
    assert(mc_keystore_count(&fx.keystore) == 0);
}

/* --- MC_OP_INPUT_LEARN (button-identification learn mode) --- */

static void enroll_and_auth(mc_session_t *s, fixture_t *fx, recorder_t *rec,
                            uint8_t sk_out[MC_CRYPTO_SECRETKEY_BYTES])
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES];
    mc_crypto_keypair(pk, sk_out);
    uint8_t enroll[1 + MC_CRYPTO_PUBKEY_BYTES + 5];
    enroll[0] = MC_OP_ENROLL;
    memcpy(enroll + 1, pk, MC_CRYPTO_PUBKEY_BYTES);
    memcpy(enroll + 1 + MC_CRYPTO_PUBKEY_BYTES, "Phone", 5);
    mc_session_handle(s, &fx->app, MC_CH_AUTH, enroll, sizeof(enroll), rec_send, rec);
    do_auth(s, &fx->app, sk_out, rec);
    assert(mc_session_is_authed(s));
}

static void test_input_learn_toggles_and_defaults_off(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES];
    enroll_and_auth(&s, &fx, &rec, sk);

    /* Off until explicitly asked for — the board must not stream input
     * events for a whole ride by default (AGENTS.md #7). */
    assert(s.input_learn == false);

    rec_reset(&rec);
    uint8_t on[2] = { MC_OP_INPUT_LEARN, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, on, sizeof(on), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(s.input_learn == true);

    rec_reset(&rec);
    uint8_t off[2] = { MC_OP_INPUT_LEARN, 0 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, off, sizeof(off), rec_send, &rec);
    r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(s.input_learn == false);
}

/* The optional second byte: while set, the platform must not run handlebar
 * bindings for the presses it reports, so setting a cheat-code on the horn
 * button doesn't sound the horn once per press. */
static void test_input_learn_action_suppression(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES];
    enroll_and_auth(&s, &fx, &rec, sk);

    assert(s.input_learn_suppress_actions == false);

    /* One-byte payload keeps the old meaning, so a client written before
     * this existed behaves exactly as it did. */
    rec_reset(&rec);
    uint8_t legacy[2] = { MC_OP_INPUT_LEARN, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, legacy, sizeof(legacy), rec_send, &rec);
    assert(s.input_learn == true);
    assert(s.input_learn_suppress_actions == false);

    rec_reset(&rec);
    uint8_t suppress[3] = { MC_OP_INPUT_LEARN, 1, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, suppress, sizeof(suppress), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_OK);
    assert(s.input_learn == true);
    assert(s.input_learn_suppress_actions == true);

    /* Turning learn mode off drops suppression with it — leaving handlebar
     * controls dead after the app stopped listening would be far worse than
     * the problem this solves. */
    rec_reset(&rec);
    uint8_t off_suppressed[3] = { MC_OP_INPUT_LEARN, 0, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, off_suppressed,
                      sizeof(off_suppressed), rec_send, &rec);
    assert(s.input_learn == false);
    assert(s.input_learn_suppress_actions == false);
}

static void test_input_learn_rejected_before_auth(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};

    uint8_t on[2] = { MC_OP_INPUT_LEARN, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, on, sizeof(on), rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_UNAUTHENTICATED);
    assert(s.input_learn == false);
}

static void test_input_learn_bad_request_without_payload(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES];
    enroll_and_auth(&s, &fx, &rec, sk);

    rec_reset(&rec);
    uint8_t bare = MC_OP_INPUT_LEARN; /* no enable byte */
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, &bare, 1, rec_send, &rec);
    const rec_frame_t *r = last_frame(&rec, MC_CH_COMMAND, MC_OP_COMMAND_RESULT);
    assert(r != NULL && r->data[2] == MC_RESULT_BAD_REQUEST);
    assert(s.input_learn == false);
}

/* Learn mode must never survive the link: mc_session_init() is what runs on
 * a new connection, so it has to clear the flag. */
static void test_input_learn_cleared_by_session_init(void)
{
    fixture_t fx;
    fixture_init(&fx);
    mc_session_t s;
    mc_session_init(&s);
    recorder_t rec = {0};
    uint8_t sk[MC_CRYPTO_SECRETKEY_BYTES];
    enroll_and_auth(&s, &fx, &rec, sk);

    uint8_t on[2] = { MC_OP_INPUT_LEARN, 1 };
    mc_session_handle(&s, &fx.app, MC_CH_COMMAND, on, sizeof(on), rec_send, &rec);
    assert(s.input_learn == true);

    mc_session_init(&s); /* simulates reconnect */
    assert(s.input_learn == false);
    assert(!mc_session_is_authed(&s));
}

int main(void)
{
    test_status_readable_without_auth();
    test_control_rejected_before_auth();
    test_enroll_tofu_then_auth_then_control();
    test_starter_rejected_over_ble_even_when_authed();
    test_hazard_press_rejected_without_turn_channels();
    test_hazard_press_rejected_before_auth();
    test_hazard_press_toggles_both_when_authed();
    test_replayed_signature_fails_on_new_session();
    test_enroll_denied_when_unauth_and_store_nonempty();
    test_key_list_and_revoke();
    test_config_read_write_roundtrip_preserves_output_state();
    test_config_write_preserves_imported_mode_not_just_on_off();
    test_config_write_rejects_invalid_config();
    test_config_rejected_before_auth();
    test_ota_rejected_before_auth();
    test_ota_full_transfer_via_session();
    test_ota_begin_rejects_bad_signature();
    test_ota_reboot_blocked_while_engine_running();
    test_event_log_get_paginated();
    test_event_log_get_rejected_when_unavailable();
    test_lock_engage_logs_event();
    test_transfer_ownership_logs_event_and_wipes_log();
    test_input_learn_toggles_and_defaults_off();
    test_input_learn_action_suppression();
    test_input_learn_rejected_before_auth();
    test_input_learn_bad_request_without_payload();
    test_input_learn_cleared_by_session_init();
    return 0;
}
