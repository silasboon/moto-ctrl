#include "mc_lock.h"

#include <string.h>

#include "mc_protocol.h" /* MC_LOCK_METHOD_* wire bits */

/* First 5 wrong cheat-code entries are free (cold hands / roadside
 * tolerance); backoff only starts on the 6th. See docs/PROTOCOL.md §11. */
#define MC_LOCK_FREE_ATTEMPTS 5
/* No attempts for this long forgives the wrong-entry counter. */
#define MC_LOCK_QUIET_RESET_MS (5u * 60u * 1000u)

/* --- small helpers --- */

static bool ignition_is_live(const mc_output_engine_t *output)
{
    int ch = mc_output_find_ignition_channel(&output->config);
    if (ch < 0) {
        return false;
    }
    return mc_output_get_state(output, (uint8_t)ch);
}

/* Forces any ON ignition/starter channel off. Never blocked by the
 * immobilized flag (that flag only guards turning ON), so this always
 * succeeds. Called on entering LOCKED. */
/* Locking makes the bike dark: every channel the rider left on goes off, not
 * just ignition and starter. Otherwise the headlight they forgot about burns
 * all night on a bike that is supposedly secured.
 *
 * Running hazards survive, and only while they are actually running — a bike
 * locked at the roadside must keep flashing. mc_output blocks switching
 * anything ON while immobilized, so this is the one-shot counterpart to that
 * standing rule. */
static void force_off_on_lock(mc_output_engine_t *output)
{
    bool hazards_running = mc_output_hazard_active(output);
    for (uint8_t ch = 0; ch < MC_OUTPUT_COUNT; ch++) {
        const mc_output_channel_config_t *cfg = &output->config.channels[ch];
        if (hazards_running && cfg->hazard_member) {
            continue;
        }
        if (mc_output_get_state(output, ch)) {
            mc_output_set(output, ch, false, MC_OUT_SRC_LOCAL);
        }
    }
}

static void reset_cheatcode_runtime(mc_lock_t *lock)
{
    lock->entry_len = 0;
    lock->consecutive_wrong = 0;
    lock->backoff_until_ms = 0;
    lock->backoff_active = false;
}

static uint32_t backoff_duration_ms(uint16_t consecutive_wrong)
{
    if (consecutive_wrong <= MC_LOCK_FREE_ATTEMPTS) {
        return 0;
    }
    switch (consecutive_wrong) {
    case MC_LOCK_FREE_ATTEMPTS + 1: return 15000u;
    case MC_LOCK_FREE_ATTEMPTS + 2: return 30000u;
    default: return 60000u; /* capped */
    }
}

static void enter_locked(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms)
{
    (void)now_ms;
    lock->state = MC_LOCK_ST_LOCKED;
    if (!lock->locked_flag) {
        lock->locked_flag = true;
        lock->dirty = true;
    }
    /* Sweep BEFORE the inhibit goes up, so each channel goes off through the
     * ordinary mc_output_set() path (turning off is never blocked, but the
     * ignition's own off-edge behaviour is easier to reason about outside the
     * locked state). */
    force_off_on_lock(output);
    mc_output_set_immobilized(output, true);
}

static void enter_unlocked(mc_lock_t *lock, mc_output_engine_t *output)
{
    bool was_locked = (lock->state == MC_LOCK_ST_LOCKED);
    lock->state = MC_LOCK_ST_UNLOCKED;
    if (lock->locked_flag) {
        lock->locked_flag = false;
        lock->dirty = true;
    }
    if (was_locked) {
        mc_output_set_immobilized(output, false);
        reset_cheatcode_runtime(lock);

        /* Unlocking turns the key: the ignition output comes live, so the
         * bike is ready to start rather than needing a second, separate
         * action the rider has no reason to expect.
         *
         * After the inhibit is lifted, or mc_output_set() would refuse its
         * own unlock. This is an ordinary command, so everything downstream
         * follows for free — on_with_ignition companions light, and the
         * starter's guards are untouched (unlocking never cranks anything).
         *
         * If an ignition-switch input is configured it stays authoritative in
         * the sense that turning it off drops the ignition again through the
         * normal path; this only decides the state at the moment of unlock. */
        int ign = mc_output_find_ignition_channel(&output->config);
        if (ign >= 0) {
            mc_output_set(output, (uint8_t)ign, true, MC_OUT_SRC_LOCAL);
        }
    }
}

static void enter_parked(mc_lock_t *lock, uint32_t now_ms)
{
    lock->state = MC_LOCK_ST_PARKED;
    lock->parked_since_ms = now_ms;
}

static void enter_disabled(mc_lock_t *lock, mc_output_engine_t *output)
{
    bool was_locked = (lock->state == MC_LOCK_ST_LOCKED);
    lock->state = MC_LOCK_ST_DISABLED;
    if (lock->locked_flag) {
        lock->locked_flag = false;
        lock->dirty = true;
    }
    if (was_locked) {
        mc_output_set_immobilized(output, false);
        reset_cheatcode_runtime(lock);
    }
}

/* --- config --- */

void mc_lock_config_default(mc_lock_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->immobilizer_enabled = false;
    out->methods_mask = 0;
    out->ignition_switch_input = -1;
    out->auto_lock_grace_ms = MC_LOCK_DEFAULT_AUTO_LOCK_GRACE_MS;
    out->cheatcode_window_ms = MC_LOCK_DEFAULT_CHEATCODE_WINDOW_MS;
    out->cheatcode_set = false;
    out->cheatcode_len = 0;
}

uint32_t mc_lock_config_validate(const mc_lock_config_t *cfg, const mc_output_config_t *outputs)
{
    uint32_t flags = 0;
    if (cfg->immobilizer_enabled) {
        /* Either fallback will do, but there must be one. An ignition-switch
         * input only counts if the method is actually enabled AND an input is
         * assigned — a mask bit pointing at nothing is not a way in. */
        bool has_switch = (cfg->methods_mask & MC_LOCK_METHOD_IGNITION_SWITCH) != 0 &&
                          cfg->ignition_switch_input >= 0 &&
                          cfg->ignition_switch_input < MC_INPUT_COUNT;
        if (!cfg->cheatcode_set && !has_switch) {
            flags |= MC_LOCK_CFG_ENABLE_REQUIRES_FALLBACK;
        }
        if (mc_output_find_ignition_channel(outputs) < 0) {
            flags |= MC_LOCK_CFG_ENABLE_REQUIRES_IGNITION_CHANNEL;
        }
    }
    if ((cfg->methods_mask & MC_LOCK_METHOD_IGNITION_SWITCH) != 0) {
        if (cfg->ignition_switch_input < 0 || cfg->ignition_switch_input >= MC_INPUT_COUNT) {
            flags |= MC_LOCK_CFG_BAD_IGNITION_SWITCH_INPUT;
        }
    }
    return flags;
}

/* --- boot / tick --- */

void mc_lock_init(mc_lock_t *lock, const mc_lock_config_t *config, bool persisted_locked_flag,
                  mc_output_engine_t *output, uint32_t now_ms)
{
    memset(lock, 0, sizeof(*lock));
    lock->config = *config;

    if (!config->immobilizer_enabled) {
        lock->state = MC_LOCK_ST_DISABLED;
        lock->locked_flag = false;
        mc_output_set_immobilized(output, false);
        return;
    }

    bool ride_running_or_live = output->engine_running || ignition_is_live(output);
    bool ride_safe_override = persisted_locked_flag && ride_running_or_live;

    if (persisted_locked_flag && !ride_safe_override) {
        lock->state = MC_LOCK_ST_LOCKED;
        lock->locked_flag = true;
        mc_output_set_immobilized(output, true);
        /* mc_output_restore_from_config() (which ran before mc_lock_init,
         * per boot order) should already have restored everything as OFF,
         * since entering LOCKED forces + persists the whole board off.
         * Re-assert defensively in case config and lock state ever drift.
         *
         * Turning OFF is permitted while immobilized, so the order here is
         * harmless. Hazards cannot be running this early — the engine was
         * zeroed at init — so nothing is exempt and the bike comes up dark,
         * which is what a locked bike should do after a power cut. */
        force_off_on_lock(output);
    } else {
        lock->state = MC_LOCK_ST_UNLOCKED;
        lock->locked_flag = false;
        mc_output_set_immobilized(output, false);
        if (ride_safe_override) {
            /* AGENTS.md #1: a brownout mid-ride must restore the bike as
             * running, never immobilized. Persist the corrected state so a
             * follow-up reboot (with the engine genuinely off) doesn't
             * re-fight this decision. */
            lock->dirty = true;
        }
    }
}

void mc_lock_tick(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms,
                  const mc_lock_inputs_t *inputs)
{
    if (lock->backoff_active && now_ms >= lock->backoff_until_ms) {
        lock->backoff_active = false;
    }
    if (lock->consecutive_wrong > 0 && !lock->backoff_active &&
        mc_elapsed_at_least(now_ms, lock->last_attempt_ms, MC_LOCK_QUIET_RESET_MS)) {
        lock->consecutive_wrong = 0;
    }
    if (lock->entry_len > 0 &&
        mc_elapsed_at_least(now_ms, lock->entry_start_ms, lock->config.cheatcode_window_ms)) {
        lock->entry_len = 0; /* timed out; not counted as a wrong entry */
    }

    if (!lock->config.immobilizer_enabled) {
        if (lock->state != MC_LOCK_ST_DISABLED) {
            enter_disabled(lock, output);
        }
        return;
    }

    /* Ignition-switch mode: outside LOCKED, a *transition* of the switch
     * drives the ignition output itself, not just an unlock trigger.
     * Turning the key off must drop ignition through the ordinary output
     * path so parked_guard below sees it and the auto-lock grace timer can
     * start — otherwise the switch could unlock the bike but never lock it
     * again (this is what the edge fires on, off->on or on->off).
     *
     * Deliberately an edge, not a level: AGENTS.md #3's methods compose as
     * OR, never AND. A rider with both phone-as-key and an ignition switch
     * configured who unlocks from the phone while the switch is sitting off
     * must get ignition — a level check here would immediately fight that
     * back off every tick, turning "one more optional unlock method" into
     * "the switch must also be on", which is exactly backwards. Only an
     * actual movement of the switch should act; a switch that's simply
     * off-and-staying-off is silent, not authoritative.
     *
     * prev_ignition_switch_level is tracked unconditionally (below, every
     * tick) so it's never stale by the time this becomes eligible to act —
     * only the ACTION is gated by switch_active/not-LOCKED, not the
     * tracking. While LOCKED the switch is a level checked below instead:
     * turning it on unlocks (enter_unlocked() lights ignition itself);
     * turning it off is already a no-op since ignition is already off. */
    bool switch_now = inputs->ignition_switch_level;
    bool switch_edge = switch_now != lock->prev_ignition_switch_level;
    lock->prev_ignition_switch_level = switch_now;

    bool switch_active = (lock->config.methods_mask & MC_LOCK_METHOD_IGNITION_SWITCH) != 0 &&
                         lock->config.ignition_switch_input >= 0;
    if (switch_active && switch_edge && lock->state != MC_LOCK_ST_LOCKED) {
        int ign = mc_output_find_ignition_channel(&output->config);
        if (ign >= 0 && switch_now != mc_output_get_state(output, (uint8_t)ign)) {
            mc_output_set(output, (uint8_t)ign, switch_now, MC_OUT_SRC_LOCAL);
        }
    }

    bool parked_guard = !inputs->engine_running && !ignition_is_live(output);

    switch (lock->state) {
    case MC_LOCK_ST_DISABLED:
        /* Normally mc_lock_apply_config() transitions straight to
         * UNLOCKED when immobilizer_enabled flips true; this is a
         * defensive catch-all in case config was mutated some other way. */
        lock->state = MC_LOCK_ST_UNLOCKED;
        break;

    case MC_LOCK_ST_UNLOCKED:
        if (parked_guard) {
            enter_parked(lock, now_ms);
        }
        break;

    case MC_LOCK_ST_PARKED:
        if (!parked_guard) {
            lock->state = MC_LOCK_ST_UNLOCKED;
        } else if (mc_elapsed_at_least(now_ms, lock->parked_since_ms, lock->config.auto_lock_grace_ms)) {
            enter_locked(lock, output, now_ms);
        }
        break;

    case MC_LOCK_ST_LOCKED:
        /* Phone-as-key unlock is edge-triggered (mc_lock_request_unlock(),
         * called from mc_app_t.on_session_authed) — see mc_lock.h. Only the
         * ignition switch is a level checked here. */
        if (inputs->ignition_switch_level &&
            (lock->config.methods_mask & MC_LOCK_METHOD_IGNITION_SWITCH) != 0) {
            enter_unlocked(lock, output);
        }
        break;
    }
}

/* --- explicit commands --- */

mc_lock_result_t mc_lock_request_lock(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms)
{
    if (!lock->config.immobilizer_enabled) {
        return MC_LOCK_RESULT_UNAUTHORIZED;
    }
    if (lock->state == MC_LOCK_ST_LOCKED) {
        return MC_LOCK_RESULT_OK; /* idempotent */
    }
    if (output->engine_running || ignition_is_live(output)) {
        return MC_LOCK_RESULT_REJECTED;
    }
    enter_locked(lock, output, now_ms);
    return MC_LOCK_RESULT_OK;
}

mc_lock_result_t mc_lock_request_unlock(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms)
{
    (void)now_ms;
    if (lock->state != MC_LOCK_ST_LOCKED) {
        return MC_LOCK_RESULT_OK; /* nothing to do */
    }
    if ((lock->config.methods_mask & MC_LOCK_METHOD_PHONE) == 0) {
        return MC_LOCK_RESULT_UNAUTHORIZED;
    }
    enter_unlocked(lock, output);
    return MC_LOCK_RESULT_OK;
}

/* --- cheat-code --- */

bool mc_lock_test_cheatcode(const mc_lock_config_t *cfg, const uint8_t *buttons, uint8_t len)
{
    if (!cfg->cheatcode_set || len == 0 || len != cfg->cheatcode_len) {
        return false;
    }
    uint8_t msg[MC_LOCK_SALT_BYTES + 1 + MC_LOCK_CHEATCODE_MAX_LEN];
    memcpy(msg, cfg->cheatcode_salt, MC_LOCK_SALT_BYTES);
    msg[MC_LOCK_SALT_BYTES] = len;
    memcpy(msg + MC_LOCK_SALT_BYTES + 1, buttons, len);

    uint8_t hash[MC_CRYPTO_HASH_BYTES];
    if (!mc_crypto_hash_sha512(msg, (size_t)MC_LOCK_SALT_BYTES + 1u + len, hash)) {
        return false;
    }
    return memcmp(hash, cfg->cheatcode_hash, MC_CRYPTO_HASH_BYTES) == 0;
}

mc_lock_cheatcode_outcome_t mc_lock_cheatcode_press(mc_lock_t *lock, mc_output_engine_t *output,
                                                    uint32_t now_ms, uint8_t button)
{
    /* Cheat-code entry is only meaningful while LOCKED — evaluating it in
     * any other state would let ordinary riding (turn signal / horn button
     * presses, on these same 8 buttons) accumulate spurious "wrong entry"
     * counts against a bike that was never locked. */
    if (lock->state != MC_LOCK_ST_LOCKED) {
        return MC_LOCK_CHEATCODE_PENDING;
    }
    if (!lock->config.cheatcode_set) {
        return MC_LOCK_CHEATCODE_NOT_SET;
    }
    if (lock->backoff_active) {
        return MC_LOCK_CHEATCODE_IN_BACKOFF;
    }
    if (button >= MC_INPUT_COUNT) {
        return MC_LOCK_CHEATCODE_PENDING;
    }

    if (lock->entry_len == 0 ||
        mc_elapsed_at_least(now_ms, lock->entry_start_ms, lock->config.cheatcode_window_ms)) {
        lock->entry_len = 0;
        lock->entry_start_ms = now_ms;
    }
    if (lock->entry_len < MC_LOCK_CHEATCODE_MAX_LEN) {
        lock->entry_buf[lock->entry_len++] = button;
    }

    if (lock->entry_len < lock->config.cheatcode_len) {
        return MC_LOCK_CHEATCODE_PENDING;
    }

    bool match = mc_lock_test_cheatcode(&lock->config, lock->entry_buf, lock->entry_len);
    lock->entry_len = 0;
    lock->last_attempt_ms = now_ms;

    if (match) {
        lock->consecutive_wrong = 0;
        lock->backoff_until_ms = 0;
        lock->backoff_active = false;
        enter_unlocked(lock, output);
        return MC_LOCK_CHEATCODE_MATCH;
    }

    lock->consecutive_wrong++;
    uint32_t dur = backoff_duration_ms(lock->consecutive_wrong);
    if (dur > 0) {
        lock->backoff_until_ms = now_ms + dur;
        lock->backoff_active = true;
    }
    return MC_LOCK_CHEATCODE_MISMATCH;
}

bool mc_lock_set_cheatcode(mc_lock_t *lock, const uint8_t *buttons, uint8_t len, uint32_t now_ms)
{
    (void)now_ms;
    if (len < MC_LOCK_CHEATCODE_MIN_LEN || len > MC_LOCK_CHEATCODE_MAX_LEN) {
        return false;
    }
    for (uint8_t i = 0; i < len; i++) {
        if (buttons[i] >= MC_INPUT_COUNT) {
            return false;
        }
    }
    uint8_t salt[MC_LOCK_SALT_BYTES];
    if (!mc_crypto_random(salt, sizeof(salt))) {
        return false;
    }
    uint8_t msg[MC_LOCK_SALT_BYTES + 1 + MC_LOCK_CHEATCODE_MAX_LEN];
    memcpy(msg, salt, MC_LOCK_SALT_BYTES);
    msg[MC_LOCK_SALT_BYTES] = len;
    memcpy(msg + MC_LOCK_SALT_BYTES + 1, buttons, len);

    uint8_t hash[MC_CRYPTO_HASH_BYTES];
    if (!mc_crypto_hash_sha512(msg, (size_t)MC_LOCK_SALT_BYTES + 1u + len, hash)) {
        return false;
    }

    memcpy(lock->config.cheatcode_salt, salt, sizeof(salt));
    memcpy(lock->config.cheatcode_hash, hash, sizeof(hash));
    lock->config.cheatcode_len = len;
    lock->config.cheatcode_set = true;
    reset_cheatcode_runtime(lock);
    lock->dirty = true;
    return true;
}

bool mc_lock_clear_cheatcode(mc_lock_t *lock, uint32_t now_ms)
{
    (void)now_ms;
    if (lock->config.immobilizer_enabled) {
        return false; /* mandatory fallback while the immobilizer is on */
    }
    memset(lock->config.cheatcode_salt, 0, sizeof(lock->config.cheatcode_salt));
    memset(lock->config.cheatcode_hash, 0, sizeof(lock->config.cheatcode_hash));
    lock->config.cheatcode_len = 0;
    lock->config.cheatcode_set = false;
    reset_cheatcode_runtime(lock);
    lock->dirty = true;
    return true;
}

/* --- config apply / ownership --- */

uint32_t mc_lock_apply_config(mc_lock_t *lock, mc_output_engine_t *output, const mc_output_config_t *outputs,
                              bool immobilizer_enabled, uint8_t methods_mask, int8_t ignition_switch_input,
                              uint32_t auto_lock_grace_ms, uint32_t cheatcode_window_ms, uint32_t now_ms)
{
    (void)now_ms;
    mc_lock_config_t candidate = lock->config;
    candidate.immobilizer_enabled = immobilizer_enabled;
    candidate.methods_mask = methods_mask;
    candidate.ignition_switch_input = ignition_switch_input;
    candidate.auto_lock_grace_ms = auto_lock_grace_ms;
    candidate.cheatcode_window_ms = cheatcode_window_ms;

    uint32_t flags = mc_lock_config_validate(&candidate, outputs);
    if (flags != MC_LOCK_CFG_OK) {
        return flags;
    }

    bool was_enabled = lock->config.immobilizer_enabled;
    lock->config = candidate;
    lock->dirty = true;

    if (!immobilizer_enabled && was_enabled) {
        enter_disabled(lock, output);
    } else if (immobilizer_enabled && !was_enabled) {
        lock->state = MC_LOCK_ST_UNLOCKED;
    }
    /* Still-enabled or still-disabled: a change to method toggles / grace
     * period alone doesn't move the state machine (an active LOCKED stays
     * LOCKED, e.g. so disabling the phone method doesn't itself unlock). */
    return MC_LOCK_CFG_OK;
}

void mc_lock_transfer_ownership(mc_lock_t *lock, mc_output_engine_t *output, uint32_t now_ms)
{
    (void)now_ms;
    enter_disabled(lock, output);
    mc_lock_config_default(&lock->config);
    lock->dirty = true;
}

/* --- accessors --- */

mc_lock_state_t mc_lock_wire_state(const mc_lock_t *lock)
{
    switch (lock->state) {
    case MC_LOCK_ST_PARKED: return MC_LOCK_PARKED;
    case MC_LOCK_ST_LOCKED: return MC_LOCK_LOCKED;
    case MC_LOCK_ST_DISABLED:
    case MC_LOCK_ST_UNLOCKED:
    default:
        return MC_LOCK_UNLOCKED;
    }
}

/* --- persistence --- */

#define MC_LOCK_MAGIC 0x314C434Du /* "MCL1" little-endian */
#define MC_LOCK_HEADER_LEN 4u
#define MC_LOCK_SCHEMA_VERSION 1

typedef struct {
    mc_lock_config_t config;
    bool locked_flag;
} mc_lock_persisted_t;

mc_lock_store_result_t mc_lock_serialize(const mc_lock_t *lock, uint8_t *buf, size_t buf_len, size_t *out_len)
{
    mc_lock_persisted_t persisted;
    memset(&persisted, 0, sizeof(persisted));
    persisted.config = lock->config;
    persisted.locked_flag = lock->locked_flag;

    size_t total = MC_LOCK_HEADER_LEN + sizeof(uint16_t) + sizeof(persisted);
    if (buf_len < total) {
        return MC_LOCK_STORE_ERR_BUFFER_TOO_SMALL;
    }

    uint32_t magic = MC_LOCK_MAGIC;
    uint16_t version = MC_LOCK_SCHEMA_VERSION;
    memcpy(buf, &magic, sizeof(magic));
    memcpy(buf + MC_LOCK_HEADER_LEN, &version, sizeof(version));
    memcpy(buf + MC_LOCK_HEADER_LEN + sizeof(version), &persisted, sizeof(persisted));

    *out_len = total;
    return MC_LOCK_STORE_OK;
}

mc_lock_store_result_t mc_lock_deserialize(const uint8_t *buf, size_t len, mc_lock_config_t *out_config,
                                           bool *out_locked_flag)
{
    if (len < MC_LOCK_HEADER_LEN + sizeof(uint16_t)) {
        return MC_LOCK_STORE_ERR_CORRUPT;
    }

    uint32_t magic;
    memcpy(&magic, buf, sizeof(magic));
    if (magic != MC_LOCK_MAGIC) {
        return MC_LOCK_STORE_ERR_CORRUPT;
    }

    uint16_t version;
    memcpy(&version, buf + MC_LOCK_HEADER_LEN, sizeof(version));
    if (version > MC_LOCK_SCHEMA_VERSION) {
        return MC_LOCK_STORE_ERR_FUTURE_VERSION;
    }

    mc_lock_persisted_t persisted;
    if (len != MC_LOCK_HEADER_LEN + sizeof(version) + sizeof(persisted)) {
        return MC_LOCK_STORE_ERR_CORRUPT;
    }
    memcpy(&persisted, buf + MC_LOCK_HEADER_LEN + sizeof(version), sizeof(persisted));

    *out_config = persisted.config;
    *out_locked_flag = persisted.locked_flag;
    return MC_LOCK_STORE_OK;
}
