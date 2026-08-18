#pragma once

/*
 * mc_power — the parked/idle power policy.
 *
 * Decides how hard the platform is allowed to idle: how often the app loop
 * ticks, whether light sleep may be entered, and how fast BLE advertises.
 * Portable C99 with no ESP-IDF dependency, exactly like mc_output/mc_diag —
 * the policy is host-testable and the chip-specific part (esp_pm, the BLE
 * advertising interval, GPIO wake sources) lives behind firmware/main's
 * power_hal.
 *
 * Light sleep rather than deep sleep, deliberately. Deep sleep would draw
 * less, but it needs an RTC-capable GPIO to wake on a handlebar button, and
 * BTN1-8 sit on GPIO35-42 (board_config.h) — outside the ESP32-S3's RTC
 * domain, which is GPIO0-21. It would also drop the BLE radio entirely, so
 * phone-as-key could never wake the bike. Light sleep keeps the radio
 * advertising and lets any GPIO wake the CPU, which is what layered unlock
 * needs: both the phone and the buttons stay live while parked.
 *
 * Three levels:
 *   - ACTIVE — something is happening. Full-rate tick, no sleep. Every
 *     timing-sensitive behaviour (flasher patterns, debounce, turn
 *     auto-cancel) only ever runs here, which is why they are unaffected by
 *     anything in this module.
 *   - IDLE   — nothing happening, but recently was, or a client is
 *     connected. Slower tick, light sleep allowed, still advertising fast so
 *     a reconnect stays quick.
 *   - PARKED — idle for a while with no client. Slowest tick, slow
 *     advertising, light sleep allowed.
 *
 * The hold-awake inputs are safety gates, not optimizations. While any of
 * them is set the policy pins itself to ACTIVE and refuses sleep:
 *
 *   - outputs_active — ride-safe failure. Never idle down while anything is
 *     being driven: blink/flasher phase and the turn auto-cancel timer are
 *     advanced by mc_output_tick(), so a slow tick would visibly break them,
 *     and a light-sleep entry that did not hold the PROFET pins would drop
 *     the load outright.
 *   - engine_running / ignition_live — the bike is in use. Covers the same
 *     rule from the other direction, and keeps mc_diag's engine-detection
 *     signals (mc_diag.h) sampling at full rate.
 *   - ota_active — an in-flight update must not have its transfer loop
 *     slowed or its flash task descheduled behind a sleeping CPU.
 *   - input_pending — a button is down, a gesture is in flight, or an event
 *     is queued. Layered unlock: sleeping mid-cheat-code would swallow the
 *     entry, and sleeping during the double-press gap would swallow the
 *     gesture.
 *   - factory_reset_armed — the boot-time arming window is still open and
 *     is polled from the same loop.
 *
 * Waking is not this module's job. A slow tick alone would miss a button
 * press entirely — mc_input_poll() only sees the pin at poll time, and a
 * human press is far shorter than the PARKED interval — so the platform
 * must also wake the loop from a GPIO interrupt. mc_input's timing is all
 * elapsed-time comparisons rather than a fixed cadence, so it is correct
 * across an irregular tick as long as it gets polled promptly once the
 * press arrives.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MC_POWER_ACTIVE = 0,
    MC_POWER_IDLE,
    MC_POWER_PARKED,
} mc_power_state_t;

/* Advertising rate class. The actual intervals are a platform concern
 * (power_hal / ble_app); this only says which one the policy wants. */
typedef enum {
    MC_POWER_ADV_FAST = 0,
    MC_POWER_ADV_SLOW,
} mc_power_adv_t;

/* What the platform should apply. Recomputed every tick; compare against
 * the previous one (mc_power_take_profile_change()) rather than reapplying
 * blindly, since changing the advertising interval restarts advertising. */
typedef struct {
    uint16_t tick_interval_ms;
    mc_power_adv_t adv;
    bool light_sleep_allowed;
} mc_power_profile_t;

/* Everything the policy needs, assembled by the platform each tick —
 * mc_power never reaches into other modules' state, matching how
 * mc_lock_inputs_t is built by the caller. */
typedef struct {
    bool outputs_active;      /* any channel commanded or actually driven */
    bool engine_running;
    bool ignition_live;
    bool ota_active;          /* mc_ota state != IDLE */
    bool session_connected;   /* at least one BLE client attached */
    bool input_pending;       /* mc_input_activity_pending() */
    bool factory_reset_armed;
} mc_power_inputs_t;

typedef struct {
    uint32_t idle_after_ms;   /* ACTIVE -> IDLE once quiet this long */
    uint32_t parked_after_ms; /* ACTIVE -> PARKED once quiet this long */
} mc_power_config_t;

/* Tick rates per level. ACTIVE keeps the historical 10ms loop, so nothing
 * about the bike in use changes. The others are the whole point: at 10ms
 * the CPU never gets an idle window long enough for light sleep to pay for
 * its own entry/exit cost. */
#define MC_POWER_TICK_ACTIVE_MS 10u
#define MC_POWER_TICK_IDLE_MS 100u
/* Deliberately not slower. Light sleep between 250ms ticks already cuts the
 * CPU's duty cycle by ~25x against the 10ms loop, and the extra saving from
 * a full second is marginal — while the cost is not: mc_input only observes
 * a button at poll time, so the tick is also the worst-case guarantee that
 * a press is seen at all if the GPIO wake path ever fails to shorten it. */
#define MC_POWER_TICK_PARKED_MS 250u

#define MC_POWER_DEFAULT_IDLE_AFTER_MS 5000u
#define MC_POWER_DEFAULT_PARKED_AFTER_MS 60000u

typedef struct {
    mc_power_config_t config;
    mc_power_state_t state;
    uint32_t last_active_ms;
    mc_power_profile_t profile;
    bool profile_changed;
} mc_power_t;

void mc_power_config_default(mc_power_config_t *out);

/* Starts in ACTIVE with sleep refused, so the boot path (output restore,
 * lock init, the factory-reset window) always runs at full rate. */
void mc_power_init(mc_power_t *p, const mc_power_config_t *config, uint32_t now_ms);

void mc_power_tick(mc_power_t *p, const mc_power_inputs_t *in, uint32_t now_ms);

static inline mc_power_state_t mc_power_get_state(const mc_power_t *p)
{
    return p->state;
}

static inline const mc_power_profile_t *mc_power_get_profile(const mc_power_t *p)
{
    return &p->profile;
}

/* True once per change, clearing the flag — so the caller only touches the
 * radio and the PM locks when something actually moved. */
bool mc_power_take_profile_change(mc_power_t *p);
