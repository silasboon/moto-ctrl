#pragma once

/*
 * mc_crypto — thin portable wrapper over the vendored TweetNaCl Ed25519
 * implementation, plus a platform CSPRNG.
 *
 * Phone-as-key model (AGENTS.md safety requirement #4): the phone holds a
 * private key in its secure keystore and signs; the device stores only
 * public keys and verifies. So on the firmware the *verify* path
 * (mc_crypto_verify) and the RNG (for nonces) are what matter; sign and
 * keypair exist for host tests that stand in for the phone, and are not
 * called by firmware.
 *
 * Signatures are detached (64-byte signature separate from the message),
 * matching tweetnacl-js's `nacl.sign.detached` on the phone side.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MC_CRYPTO_PUBKEY_BYTES 32
#define MC_CRYPTO_SECRETKEY_BYTES 64 /* TweetNaCl Ed25519 secret key layout */
#define MC_CRYPTO_SIG_BYTES 64
#define MC_CRYPTO_NONCE_BYTES 32
#define MC_CRYPTO_HASH_BYTES 64 /* SHA-512 digest */

/* Longest message the sign/verify wrappers accept. The auth message is a
 * short context string + a 32-byte nonce, so this is comfortably large. */
#define MC_CRYPTO_MAX_MSG 128

/* Fills out[0..len) with cryptographically secure random bytes. Returns
 * false if the platform RNG failed (callers must treat this as fatal for
 * anything security-relevant). */
bool mc_crypto_random(uint8_t *out, size_t len);

/* Verifies a detached Ed25519 signature. Returns true iff `sig` is a valid
 * signature of msg[0..msg_len) under `pubkey`. msg_len must be
 * <= MC_CRYPTO_MAX_MSG. Constant-time in the sense TweetNaCl provides;
 * does not early-return on content (only on length bounds). */
bool mc_crypto_verify(const uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES],
                      const uint8_t *msg, size_t msg_len,
                      const uint8_t sig[MC_CRYPTO_SIG_BYTES]);

/* SHA-512 of msg[0..msg_len). Used by mc_lock to store a salted
 * hash of the button cheat-code rather than the plaintext sequence — see
 * AGENTS.md safety requirement #4's "device never stores anything that
 * could unlock it if the flash is dumped" doctrine, applied here to the
 * cheat-code as well as to enrolled keys. Not secret-dependent timing is
 * not a concern here (unlike mc_crypto_verify): this is a general-purpose
 * hash, not a MAC. */
bool mc_crypto_hash_sha512(const uint8_t *msg, size_t msg_len, uint8_t out[MC_CRYPTO_HASH_BYTES]);

/* Incremental SHA-512, for hashing data too large to buffer in one call —
 * mc_ota streams a firmware image (up to 1.5MB) through this a
 * chunk at a time rather than holding it all in RAM (this hardware has no
 * PSRAM). mc_crypto_hash_sha512() above is a thin wrapper over
 * init/update/final and always produces the same digest as calling these
 * three directly. */
typedef struct {
    uint8_t h[MC_CRYPTO_HASH_BYTES];  /* running SHA-512 state */
    uint8_t block[128];               /* partial-block buffer (< 128 bytes) */
    size_t block_len;
    uint64_t total_len;               /* total bytes fed in, for the length suffix */
} mc_crypto_hash_ctx_t;

void mc_crypto_hash_sha512_init(mc_crypto_hash_ctx_t *ctx);
void mc_crypto_hash_sha512_update(mc_crypto_hash_ctx_t *ctx, const uint8_t *data, size_t len);
void mc_crypto_hash_sha512_final(mc_crypto_hash_ctx_t *ctx, uint8_t out[MC_CRYPTO_HASH_BYTES]);

/* --- Host/test only (stands in for the phone). Not used by firmware. --- */

/* Generates an Ed25519 keypair. Uses the platform CSPRNG. */
bool mc_crypto_keypair(uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES],
                       uint8_t secret[MC_CRYPTO_SECRETKEY_BYTES]);

/* Produces a detached Ed25519 signature of msg[0..msg_len) under `secret`.
 * msg_len must be <= MC_CRYPTO_MAX_MSG. Returns false on bad length. */
bool mc_crypto_sign(uint8_t sig[MC_CRYPTO_SIG_BYTES],
                    const uint8_t *msg, size_t msg_len,
                    const uint8_t secret[MC_CRYPTO_SECRETKEY_BYTES]);
