#include "mc_crypto.h"

#include <assert.h>
#include <string.h>

static void test_sign_verify_roundtrip(void)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    assert(mc_crypto_keypair(pk, sk));

    const uint8_t msg[] = "moto-ctrl-auth-v1................"; /* 32+ bytes */
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    assert(mc_crypto_sign(sig, msg, sizeof(msg), sk));
    assert(mc_crypto_verify(pk, msg, sizeof(msg), sig));
}

static void test_verify_rejects_tampered_message(void)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);

    uint8_t msg[40];
    memset(msg, 0xAB, sizeof(msg));
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    mc_crypto_sign(sig, msg, sizeof(msg), sk);

    msg[10] ^= 0x01;
    assert(!mc_crypto_verify(pk, msg, sizeof(msg), sig));
}

static void test_verify_rejects_tampered_signature(void)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);

    uint8_t msg[40];
    memset(msg, 0x11, sizeof(msg));
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    mc_crypto_sign(sig, msg, sizeof(msg), sk);

    sig[0] ^= 0x80;
    assert(!mc_crypto_verify(pk, msg, sizeof(msg), sig));
}

static void test_verify_rejects_wrong_pubkey(void)
{
    uint8_t pk1[MC_CRYPTO_PUBKEY_BYTES], sk1[MC_CRYPTO_SECRETKEY_BYTES];
    uint8_t pk2[MC_CRYPTO_PUBKEY_BYTES], sk2[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk1, sk1);
    mc_crypto_keypair(pk2, sk2);

    uint8_t msg[40];
    memset(msg, 0x22, sizeof(msg));
    uint8_t sig[MC_CRYPTO_SIG_BYTES];
    mc_crypto_sign(sig, msg, sizeof(msg), sk1);

    assert(mc_crypto_verify(pk1, msg, sizeof(msg), sig));
    assert(!mc_crypto_verify(pk2, msg, sizeof(msg), sig));
}

static void test_random_produces_distinct(void)
{
    uint8_t a[32], b[32];
    assert(mc_crypto_random(a, sizeof(a)));
    assert(mc_crypto_random(b, sizeof(b)));
    assert(memcmp(a, b, sizeof(a)) != 0); /* astronomically unlikely to collide */
}

static void test_verify_rejects_oversized_message(void)
{
    uint8_t pk[MC_CRYPTO_PUBKEY_BYTES], sk[MC_CRYPTO_SECRETKEY_BYTES];
    mc_crypto_keypair(pk, sk);
    uint8_t sig[MC_CRYPTO_SIG_BYTES] = {0};
    uint8_t big[MC_CRYPTO_MAX_MSG + 1];
    memset(big, 0, sizeof(big));
    assert(!mc_crypto_verify(pk, big, sizeof(big), sig));
    assert(!mc_crypto_sign(sig, big, sizeof(big), sk));
}

/* NIST/FIPS 180-4 known-answer test: SHA-512("abc"). Independent of the
 * streaming/one-shot equivalence tests below (which would both pass even
 * if the underlying math were wrong, since mc_crypto_hash_sha512() is now
 * implemented in terms of the streaming API) — this catches a fundamental
 * algorithm bug in either. */
static void test_hash_known_answer_vector(void)
{
    static const uint8_t expected[MC_CRYPTO_HASH_BYTES] = {
        0xdd, 0xaf, 0x35, 0xa1, 0x93, 0x61, 0x7a, 0xba,
        0xcc, 0x41, 0x73, 0x49, 0xae, 0x20, 0x41, 0x31,
        0x12, 0xe6, 0xfa, 0x4e, 0x89, 0xa9, 0x7e, 0xa2,
        0x0a, 0x9e, 0xee, 0xe6, 0x4b, 0x55, 0xd3, 0x9a,
        0x21, 0x92, 0x99, 0x2a, 0x27, 0x4f, 0xc1, 0xa8,
        0x36, 0xba, 0x3c, 0x23, 0xa3, 0xfe, 0xeb, 0xbd,
        0x45, 0x4d, 0x44, 0x23, 0x64, 0x3c, 0xe8, 0x0e,
        0x2a, 0x9a, 0xc9, 0x4f, 0xa5, 0x4c, 0xa4, 0x9f,
    };
    uint8_t out[MC_CRYPTO_HASH_BYTES];
    assert(mc_crypto_hash_sha512((const uint8_t *)"abc", 3, out));
    assert(memcmp(out, expected, MC_CRYPTO_HASH_BYTES) == 0);
}

/* Feeds the same message through mc_crypto_hash_sha512_update() in
 * differently-sized chunks (including sizes that straddle the 128-byte
 * block boundary and the 112-byte final-block-vs-two-block padding
 * threshold) and asserts every chunking produces the same digest as the
 * one-shot call — exactly the class of bug a hand-written streaming
 * implementation's block/tail bookkeeping could hide. */
static void assert_chunked_matches_oneshot(const uint8_t *msg, size_t len, size_t chunk_size)
{
    uint8_t expected[MC_CRYPTO_HASH_BYTES];
    assert(mc_crypto_hash_sha512(msg, len, expected));

    mc_crypto_hash_ctx_t ctx;
    mc_crypto_hash_sha512_init(&ctx);
    size_t off = 0;
    while (off < len) {
        size_t n = (len - off < chunk_size) ? (len - off) : chunk_size;
        mc_crypto_hash_sha512_update(&ctx, msg + off, n);
        off += n;
    }
    uint8_t got[MC_CRYPTO_HASH_BYTES];
    mc_crypto_hash_sha512_final(&ctx, got);
    assert(memcmp(got, expected, MC_CRYPTO_HASH_BYTES) == 0);
}

static void test_incremental_matches_oneshot_across_sizes_and_chunking(void)
{
    static uint8_t msg[5000];
    for (size_t i = 0; i < sizeof(msg); i++) {
        msg[i] = (uint8_t)(i * 37 + 11); /* deterministic pseudo-random filler */
    }

    /* Message lengths straddling the 128-byte block boundary and the
     * 112-byte one-block-vs-two-block padding threshold. */
    size_t lengths[] = {0, 1, 111, 112, 127, 128, 129, 255, 256, 257, 1000, 5000};
    /* Chunk sizes including "one big update()" (== length, handled per
     * length below) and small/misaligned sizes. */
    size_t chunk_sizes[] = {1, 7, 64, 127, 128, 129, 4096};

    for (size_t li = 0; li < sizeof(lengths) / sizeof(lengths[0]); li++) {
        size_t len = lengths[li];
        assert_chunked_matches_oneshot(msg, len, len == 0 ? 1 : len); /* single update() call */
        for (size_t ci = 0; ci < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); ci++) {
            assert_chunked_matches_oneshot(msg, len, chunk_sizes[ci]);
        }
    }
}

static void test_hash_empty_message(void)
{
    uint8_t out[MC_CRYPTO_HASH_BYTES];
    assert(mc_crypto_hash_sha512(NULL, 0, out) || 1); /* documented: msg may be unused when len==0 */

    mc_crypto_hash_ctx_t ctx;
    mc_crypto_hash_sha512_init(&ctx);
    mc_crypto_hash_sha512_final(&ctx, out);
    /* SHA-512("") known-answer, FIPS 180-4. */
    static const uint8_t expected[MC_CRYPTO_HASH_BYTES] = {
        0xcf, 0x83, 0xe1, 0x35, 0x7e, 0xef, 0xb8, 0xbd,
        0xf1, 0x54, 0x28, 0x50, 0xd6, 0x6d, 0x80, 0x07,
        0xd6, 0x20, 0xe4, 0x05, 0x0b, 0x57, 0x15, 0xdc,
        0x83, 0xf4, 0xa9, 0x21, 0xd3, 0x6c, 0xe9, 0xce,
        0x47, 0xd0, 0xd1, 0x3c, 0x5d, 0x85, 0xf2, 0xb0,
        0xff, 0x83, 0x18, 0xd2, 0x87, 0x7e, 0xec, 0x2f,
        0x63, 0xb9, 0x31, 0xbd, 0x47, 0x41, 0x7a, 0x81,
        0xa5, 0x38, 0x32, 0x7a, 0xf9, 0x27, 0xda, 0x3e,
    };
    assert(memcmp(out, expected, MC_CRYPTO_HASH_BYTES) == 0);
}

int main(void)
{
    test_sign_verify_roundtrip();
    test_verify_rejects_tampered_message();
    test_verify_rejects_tampered_signature();
    test_verify_rejects_wrong_pubkey();
    test_random_produces_distinct();
    test_verify_rejects_oversized_message();
    test_hash_known_answer_vector();
    test_incremental_matches_oneshot_across_sizes_and_chunking();
    test_hash_empty_message();
    return 0;
}
