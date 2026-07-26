/*
 * mc_ota — the OTA update state machine. Deterministic,
 * synthetic-HAL coverage of every state transition and rejection path,
 * mirroring the "extra test coverage — every state transition" bar
 * AGENTS.md sets for other safety-relevant modules (lock, output).
 */
#include "mc_ota.h"

#include <assert.h>
#include <string.h>

#include "mc_crypto.h"
#include "mc_protocol.h" /* MC_OTA_MAX_IMAGE_SIZE */

/* --- fake flash HAL: an in-memory buffer standing in for esp_ota_* --- */

#define FAKE_FLASH_MAX 65536

typedef struct {
    uint8_t buf[FAKE_FLASH_MAX];
    uint32_t len;
    bool begin_should_fail;
    bool write_should_fail;
    bool finalize_should_fail;
    int begin_calls, write_calls, finalize_calls, abort_calls, reboot_calls;
} fake_flash_t;

static bool flash_begin(uint32_t image_size, void *ctx)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    f->begin_calls++;
    f->len = 0;
    if (f->begin_should_fail) {
        return false;
    }
    return image_size <= FAKE_FLASH_MAX;
}
static bool flash_write(uint32_t offset, const uint8_t *data, size_t len, void *ctx)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    f->write_calls++;
    if (f->write_should_fail) {
        return false;
    }
    if ((uint64_t)offset + (uint64_t)len > FAKE_FLASH_MAX) {
        return false;
    }
    memcpy(f->buf + offset, data, len);
    if (offset + len > f->len) {
        f->len = offset + (uint32_t)len;
    }
    return true;
}
static bool flash_finalize(void *ctx)
{
    fake_flash_t *f = (fake_flash_t *)ctx;
    f->finalize_calls++;
    return !f->finalize_should_fail;
}
static void flash_abort(void *ctx) { ((fake_flash_t *)ctx)->abort_calls++; }
static void flash_reboot(void *ctx) { ((fake_flash_t *)ctx)->reboot_calls++; }

typedef struct {
    mc_ota_t ota;
    fake_flash_t flash;
    uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES];
    uint8_t secret[MC_CRYPTO_SECRETKEY_BYTES];
} fixture_t;

static void fixture_init(fixture_t *fx)
{
    memset(fx, 0, sizeof(*fx));
    mc_crypto_keypair(fx->pubkey, fx->secret);
    mc_ota_hal_t hal = {
        .flash_begin = flash_begin,
        .flash_write = flash_write,
        .flash_finalize = flash_finalize,
        .flash_abort = flash_abort,
        .reboot = flash_reboot,
        .ctx = &fx->flash,
    };
    mc_ota_init(&fx->ota, hal, fx->pubkey);
}

/* Signs sha512(image) with fx->secret -- the "release key" for this test. */
static void sign_image(fixture_t *fx, const uint8_t *image, size_t len,
                       uint8_t sha512[MC_CRYPTO_HASH_BYTES], uint8_t sig[MC_CRYPTO_SIG_BYTES])
{
    assert(mc_crypto_hash_sha512(image, len, sha512));
    assert(mc_crypto_sign(sig, sha512, MC_CRYPTO_HASH_BYTES, fx->secret));
}

static void feed_chunks(fixture_t *fx, const uint8_t *image, size_t len, size_t chunk_size)
{
    size_t off = 0;
    while (off < len) {
        size_t n = (len - off < chunk_size) ? (len - off) : chunk_size;
        assert(mc_ota_chunk(&fx->ota, (uint32_t)off, image + off, n) == MC_OTA_OK);
        off += n;
    }
}

/* --- happy path --- */

static void test_full_transfer_happy_path(void)
{
    fixture_t fx;
    fixture_init(&fx);

    uint8_t image[500];
    for (size_t i = 0; i < sizeof(image); i++) {
        image[i] = (uint8_t)(i * 7 + 3);
    }
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_RECEIVING);
    assert(fx.flash.begin_calls == 1);

    feed_chunks(&fx, image, sizeof(image), 128);
    assert(mc_ota_get_bytes_received(&fx.ota) == sizeof(image));
    assert(fx.flash.write_calls == 4); /* ceil(500/128) */

    assert(mc_ota_commit(&fx.ota) == MC_OTA_OK);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);
    assert(fx.flash.finalize_calls == 1);
    assert(fx.flash.len == sizeof(image));
    assert(memcmp(fx.flash.buf, image, sizeof(image)) == 0);

    assert(mc_ota_reboot(&fx.ota, true) == MC_OTA_OK);
    assert(fx.flash.reboot_calls == 1);
}

static void test_single_chunk_transfer(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[64] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);
    assert(mc_ota_chunk(&fx.ota, 0, image, sizeof(image)) == MC_OTA_OK);
    assert(mc_ota_commit(&fx.ota) == MC_OTA_OK);
}

/* --- signature / trust --- */

static void test_begin_rejects_bad_signature(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[64] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    sig[0] ^= 0x01; /* tamper */

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_ERR_BAD_SIGNATURE);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE); /* unchanged: no flash touched */
    assert(fx.flash.begin_calls == 0);
}

static void test_begin_rejects_signature_from_wrong_key(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t other_pk[MC_CRYPTO_PUBKEY_BYTES], other_sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(other_pk, other_sk);

    uint8_t image[64] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_hash_sha512(image, sizeof(image), sha));
    assert(mc_crypto_sign(sig, sha, MC_CRYPTO_HASH_BYTES, other_sk)); /* signed by the WRONG key */

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_ERR_BAD_SIGNATURE);
    assert(fx.flash.begin_calls == 0);
}

static void test_commit_rejects_transport_corruption(void)
{
    /* Signature verifies (it's over the DECLARED hash), but the bytes that
     * actually arrive don't match it -- must be caught at commit, not
     * silently accepted. */
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[64];
    memset(image, 0xAB, sizeof(image));
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);
    uint8_t corrupted[64];
    memset(corrupted, 0xCD, sizeof(corrupted)); /* different bytes than what was signed */
    assert(mc_ota_chunk(&fx.ota, 0, corrupted, sizeof(corrupted)) == MC_OTA_OK); /* chunk() doesn't know yet */
    assert(mc_ota_commit(&fx.ota) == MC_OTA_ERR_HASH_MISMATCH);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_ERROR);
    assert(fx.flash.finalize_calls == 0); /* never finalized a corrupted image */
}

/* --- chunk ordering / bounds --- */

static void test_out_of_order_chunk_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[64] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);

    assert(mc_ota_chunk(&fx.ota, 16, image, 16) == MC_OTA_ERR_OUT_OF_ORDER_CHUNK); /* should start at 0 */
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_ERROR);
}

static void test_overrun_chunk_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[32] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);

    uint8_t too_big[64] = {0};
    assert(mc_ota_chunk(&fx.ota, 0, too_big, sizeof(too_big)) == MC_OTA_ERR_OVERRUN);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_ERROR);
}

static void test_commit_before_all_bytes_received_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[64] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);
    mc_ota_chunk(&fx.ota, 0, image, 32); /* only half */

    assert(mc_ota_commit(&fx.ota) == MC_OTA_ERR_BAD_STATE);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_RECEIVING); /* still receiving, not errored */
}

/* --- size bounds --- */

static void test_begin_rejects_zero_size(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t sha[MC_CRYPTO_HASH_BYTES] = {0}, sig[MC_CRYPTO_SIG_BYTES] = {0};
    assert(mc_ota_begin(&fx.ota, 0, sha, sig, true) == MC_OTA_ERR_BAD_SIZE);
}

static void test_begin_rejects_oversized_image(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t sha[MC_CRYPTO_HASH_BYTES] = {0}, sig[MC_CRYPTO_SIG_BYTES] = {0};
    assert(mc_ota_begin(&fx.ota, MC_OTA_MAX_IMAGE_SIZE + 1, sha, sig, true) == MC_OTA_ERR_BAD_SIZE);
}

/* --- safe-state gating --- */

static void test_begin_rejects_unsafe_state(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[16] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, false) == MC_OTA_ERR_UNSAFE_STATE);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
    assert(fx.flash.begin_calls == 0);
}

static void test_committed_then_unsafe_then_safe_reboot_sequence(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);
    assert(mc_ota_chunk(&fx.ota, 0, image, sizeof(image)) == MC_OTA_OK);
    assert(mc_ota_commit(&fx.ota) == MC_OTA_OK);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);

    /* Engine started running between commit and reboot: refused, but the
     * COMMITTED image is not lost -- nothing about the transfer is
     * discarded, the client just retries OTA_REBOOT later. */
    assert(mc_ota_reboot(&fx.ota, false) == MC_OTA_ERR_UNSAFE_STATE);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_COMMITTED);
    assert(fx.flash.reboot_calls == 0);

    /* Engine stops: retry succeeds without re-transferring anything. */
    assert(mc_ota_reboot(&fx.ota, true) == MC_OTA_OK);
    assert(fx.flash.reboot_calls == 1);
}

/* --- abort --- */

static void test_abort_from_every_state(void)
{
    fixture_t fx;

    /* From IDLE: always safe/idempotent, no HAL side effects other than
     * flash_abort() itself. */
    fixture_init(&fx);
    mc_ota_abort(&fx.ota);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
    assert(fx.flash.abort_calls == 1);

    /* From RECEIVING. */
    fixture_init(&fx);
    uint8_t image[16] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);
    mc_ota_chunk(&fx.ota, 0, image, 8);
    mc_ota_abort(&fx.ota);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
    assert(fx.flash.abort_calls == 1);
    /* A fresh begin() works immediately after abort. */
    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);

    /* From COMMITTED. */
    fixture_init(&fx);
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);
    mc_ota_chunk(&fx.ota, 0, image, sizeof(image));
    mc_ota_commit(&fx.ota);
    mc_ota_abort(&fx.ota);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);

    /* From ERROR (e.g. after an out-of-order chunk). */
    fixture_init(&fx);
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);
    mc_ota_chunk(&fx.ota, 8, image, 8); /* wrong offset -> ERROR */
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_ERROR);
    mc_ota_abort(&fx.ota);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
}

/* --- state-guard rejections --- */

static void test_double_begin_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[16] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_OK);
    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_ERR_BAD_STATE);
    assert(fx.flash.begin_calls == 1); /* second call never reached the HAL */
}

static void test_chunk_before_begin_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[16] = {0};
    assert(mc_ota_chunk(&fx.ota, 0, image, sizeof(image)) == MC_OTA_ERR_BAD_STATE);
}

static void test_reboot_without_commit_rejected(void)
{
    fixture_t fx;
    fixture_init(&fx);
    assert(mc_ota_reboot(&fx.ota, true) == MC_OTA_ERR_BAD_STATE);
    assert(fx.flash.reboot_calls == 0);
}

/* --- flash HAL failure propagation --- */

static void test_flash_begin_failure_propagates(void)
{
    fixture_t fx;
    fixture_init(&fx);
    fx.flash.begin_should_fail = true;
    uint8_t image[16] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);

    assert(mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true) == MC_OTA_ERR_FLASH);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_IDLE);
}

static void test_flash_write_failure_propagates(void)
{
    fixture_t fx;
    fixture_init(&fx);
    uint8_t image[16] = {0};
    uint8_t sha[MC_CRYPTO_HASH_BYTES], sig[MC_CRYPTO_SIG_BYTES];
    sign_image(&fx, image, sizeof(image), sha, sig);
    mc_ota_begin(&fx.ota, sizeof(image), sha, sig, true);

    fx.flash.write_should_fail = true;
    assert(mc_ota_chunk(&fx.ota, 0, image, sizeof(image)) == MC_OTA_ERR_FLASH);
    assert(mc_ota_get_state(&fx.ota) == MC_OTA_ERROR);
}

int main(void)
{
    test_full_transfer_happy_path();
    test_single_chunk_transfer();
    test_begin_rejects_bad_signature();
    test_begin_rejects_signature_from_wrong_key();
    test_commit_rejects_transport_corruption();
    test_out_of_order_chunk_rejected();
    test_overrun_chunk_rejected();
    test_commit_before_all_bytes_received_rejected();
    test_begin_rejects_zero_size();
    test_begin_rejects_oversized_image();
    test_begin_rejects_unsafe_state();
    test_committed_then_unsafe_then_safe_reboot_sequence();
    test_abort_from_every_state();
    test_double_begin_rejected();
    test_chunk_before_begin_rejected();
    test_reboot_without_commit_rejected();
    test_flash_begin_failure_propagates();
    test_flash_write_failure_propagates();
    return 0;
}
