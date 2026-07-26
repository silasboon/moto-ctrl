#include "mc_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "tweetnacl.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <stdio.h>
#endif

bool mc_crypto_random(uint8_t *out, size_t len)
{
#ifdef ESP_PLATFORM
    esp_fill_random(out, len);
    return true;
#else
    /* Host build (sim + unit tests): read the OS CSPRNG. */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL) {
        return false;
    }
    size_t got = fread(out, 1, len, f);
    fclose(f);
    return got == len;
#endif
}

/* TweetNaCl requires the integrator to provide this symbol. Any failure of
 * the platform CSPRNG is fatal for key/nonce generation — abort rather than
 * silently produce predictable output. */
void randombytes(unsigned char *out, unsigned long long len)
{
    if (!mc_crypto_random(out, (size_t)len)) {
        abort();
    }
}

bool mc_crypto_verify(const uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES],
                      const uint8_t *msg, size_t msg_len,
                      const uint8_t sig[MC_CRYPTO_SIG_BYTES])
{
    if (msg_len > MC_CRYPTO_MAX_MSG) {
        return false;
    }

    /* Reconstruct TweetNaCl's signed-message format (sig || msg) and let
     * crypto_sign_open verify and recover it. */
    unsigned char sm[MC_CRYPTO_SIG_BYTES + MC_CRYPTO_MAX_MSG];
    unsigned char recovered[MC_CRYPTO_SIG_BYTES + MC_CRYPTO_MAX_MSG];
    unsigned long long smlen = (unsigned long long)(MC_CRYPTO_SIG_BYTES + msg_len);
    unsigned long long mlen = 0;

    memcpy(sm, sig, MC_CRYPTO_SIG_BYTES);
    memcpy(sm + MC_CRYPTO_SIG_BYTES, msg, msg_len);

    int rc = crypto_sign_open(recovered, &mlen, sm, smlen, pubkey);
    return rc == 0;
}

/* Standard SHA-512 initial hash value (FIPS 180-4) — the same constant
 * TweetNaCl's own crypto_hash() uses internally, kept here as our own copy
 * since crypto_hash()'s `iv` is private to tweetnacl.c. */
static const uint8_t sha512_iv[MC_CRYPTO_HASH_BYTES] = {
    0x6a, 0x09, 0xe6, 0x67, 0xf3, 0xbc, 0xc9, 0x08,
    0xbb, 0x67, 0xae, 0x85, 0x84, 0xca, 0xa7, 0x3b,
    0x3c, 0x6e, 0xf3, 0x72, 0xfe, 0x94, 0xf8, 0x2b,
    0xa5, 0x4f, 0xf5, 0x3a, 0x5f, 0x1d, 0x36, 0xf1,
    0x51, 0x0e, 0x52, 0x7f, 0xad, 0xe6, 0x82, 0xd1,
    0x9b, 0x05, 0x68, 0x8c, 0x2b, 0x3e, 0x6c, 0x1f,
    0x1f, 0x83, 0xd9, 0xab, 0xfb, 0x41, 0xbd, 0x6b,
    0x5b, 0xe0, 0xcd, 0x19, 0x13, 0x7e, 0x21, 0x79,
};

static void store_u64_be(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)(v & 0xFF);
        v >>= 8;
    }
}

void mc_crypto_hash_sha512_init(mc_crypto_hash_ctx_t *ctx)
{
    memcpy(ctx->h, sha512_iv, MC_CRYPTO_HASH_BYTES);
    ctx->block_len = 0;
    ctx->total_len = 0;
}

void mc_crypto_hash_sha512_update(mc_crypto_hash_ctx_t *ctx, const uint8_t *data, size_t len)
{
    ctx->total_len += (uint64_t)len;

    /* Top up any leftover partial block from the previous update() first,
     * so crypto_hashblocks always sees whole 128-byte blocks. */
    if (ctx->block_len > 0) {
        size_t need = 128 - ctx->block_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->block + ctx->block_len, data, take);
        ctx->block_len += take;
        data += take;
        len -= take;

        if (ctx->block_len < 128) {
            return; /* still not a full block; nothing more to do yet */
        }
        crypto_hashblocks(ctx->h, ctx->block, 128);
        ctx->block_len = 0;
    }

    /* crypto_hashblocks processes every full 128-byte block in one call and
     * returns the leftover (< 128) byte count — exactly what crypto_hash()
     * itself relies on internally. */
    if (len >= 128) {
        size_t rem = (size_t)crypto_hashblocks(ctx->h, data, len);
        size_t consumed = len - rem;
        data += consumed;
        len = rem;
    }

    if (len > 0) {
        memcpy(ctx->block, data, len);
        ctx->block_len = len;
    }
}

void mc_crypto_hash_sha512_final(mc_crypto_hash_ctx_t *ctx, uint8_t out[MC_CRYPTO_HASH_BYTES])
{
    /* Standard Merkle-Damgard padding: 0x80, zero-fill, then the total
     * bit-length as a big-endian 64-bit suffix, in one block if the
     * leftover + padding fits (< 112 bytes of leftover), else two blocks —
     * mirrors crypto_hash()'s own tail handling exactly. */
    uint8_t pad[256];
    memset(pad, 0, sizeof(pad));
    memcpy(pad, ctx->block, ctx->block_len);
    pad[ctx->block_len] = 0x80;

    size_t n = (ctx->block_len < 112) ? 128 : 256;
    uint64_t total_bits = ctx->total_len << 3; /* byte count -> bit count */
    pad[n - 9] = (uint8_t)(ctx->total_len >> 61); /* top bits, if any */
    store_u64_be(pad + n - 8, total_bits);

    crypto_hashblocks(ctx->h, pad, n);
    memcpy(out, ctx->h, MC_CRYPTO_HASH_BYTES);
}

bool mc_crypto_hash_sha512(const uint8_t *msg, size_t msg_len, uint8_t out[MC_CRYPTO_HASH_BYTES])
{
    mc_crypto_hash_ctx_t ctx;
    mc_crypto_hash_sha512_init(&ctx);
    mc_crypto_hash_sha512_update(&ctx, msg, msg_len);
    mc_crypto_hash_sha512_final(&ctx, out);
    return true;
}

bool mc_crypto_keypair(uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES],
                       uint8_t secret[MC_CRYPTO_SECRETKEY_BYTES])
{
    return crypto_sign_keypair(pubkey, secret) == 0;
}

bool mc_crypto_sign(uint8_t sig[MC_CRYPTO_SIG_BYTES],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t secret[MC_CRYPTO_SECRETKEY_BYTES])
{
    if (msg_len > MC_CRYPTO_MAX_MSG) {
        return false;
    }

    unsigned char sm[MC_CRYPTO_SIG_BYTES + MC_CRYPTO_MAX_MSG];
    unsigned long long smlen = 0;

    if (crypto_sign(sm, &smlen, msg, (unsigned long long)msg_len, secret) != 0) {
        return false;
    }
    memcpy(sig, sm, MC_CRYPTO_SIG_BYTES);
    return true;
}
