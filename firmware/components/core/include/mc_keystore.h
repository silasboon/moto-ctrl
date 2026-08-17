#pragma once

/*
 * mc_keystore — the set of enrolled phone public keys (paired phones).
 *
 * Holds ONLY Ed25519 public keys (phone-as-key auth: the
 * device never stores anything that could unlock it if the flash is
 * dumped). Supports multiple paired phones, per-key revocation, and a full
 * wipe for the ownership-transfer flow.
 *
 * Persisted separately from mc_config (its own NVS blob), and deliberately
 * NOT part of the JSON config export/import — re-importing a config backup
 * must never silently re-enroll a revoked key.
 */

#include "mc_crypto.h"
#include "mc_types.h"

#define MC_KEYSTORE_MAX_KEYS 8
#define MC_KEY_LABEL_MAX 24

typedef struct {
    bool in_use;
    uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES];
    char label[MC_KEY_LABEL_MAX];
} mc_key_slot_t;

typedef struct {
    mc_key_slot_t slots[MC_KEYSTORE_MAX_KEYS];
} mc_keystore_t;

void mc_keystore_init(mc_keystore_t *ks);

int mc_keystore_count(const mc_keystore_t *ks);
static inline bool mc_keystore_is_empty(const mc_keystore_t *ks)
{
    return mc_keystore_count(ks) == 0;
}

/* Enrolls a public key with a friendly label. If the key is already
 * present, updates its label and returns the existing slot (idempotent).
 * Returns the slot index (0..MC_KEYSTORE_MAX_KEYS-1), or -1 if the store
 * is full. `label` may be NULL. */
int mc_keystore_add(mc_keystore_t *ks, const uint8_t pubkey[MC_CRYPTO_PUBKEY_BYTES], const char *label);

/* Revokes (removes) the key in `slot`. Returns true if a key was removed. */
bool mc_keystore_remove(mc_keystore_t *ks, int slot);

/* Wipes every enrolled key (ownership transfer / factory reset). */
void mc_keystore_wipe(mc_keystore_t *ks);

/* Returns the slot index of an enrolled key whose public key produces a
 * valid signature `sig` over msg[0..msg_len), or -1 if none match. This is
 * the core of the challenge-response check. */
int mc_keystore_find_verifying(const mc_keystore_t *ks,
                               const uint8_t *msg, size_t msg_len,
                               const uint8_t sig[MC_CRYPTO_SIG_BYTES]);

/* Read accessors for the key-list command. Return false for an unused or
 * out-of-range slot. `out_pubkey` / `out_label` may be NULL. */
bool mc_keystore_get(const mc_keystore_t *ks, int slot,
                     uint8_t out_pubkey[MC_CRYPTO_PUBKEY_BYTES], char *out_label, size_t label_cap);

/* Serialize/deserialize the whole keystore as a versioned blob for NVS.
 * Same envelope style as mc_config. */
typedef enum {
    MC_KEYSTORE_OK = 0,
    MC_KEYSTORE_ERR_BUFFER_TOO_SMALL,
    MC_KEYSTORE_ERR_CORRUPT,
    MC_KEYSTORE_ERR_FUTURE_VERSION,
} mc_keystore_result_t;

mc_keystore_result_t mc_keystore_serialize(const mc_keystore_t *ks, uint8_t *buf, size_t buf_len, size_t *out_len);
mc_keystore_result_t mc_keystore_deserialize(const uint8_t *buf, size_t len, mc_keystore_t *out);
