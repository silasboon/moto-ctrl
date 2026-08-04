#include "factory_reset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs.h"

#include "mc_event_log.h"

#include "board_config.h"
#include "nvs_event_log_hal.h"
#include "output_hal_gpio.h"

static const char *TAG = "mc_factory_reset";

#define BLINK_ON_MS 120u
#define BLINK_OFF_MS 120u
#define BLINK_CYCLES 6

static void set_all_outputs(bool on)
{
    mc_output_hal_t hal = output_hal_gpio_get();
    for (uint8_t ch = 0; ch < BOARD_OUTPUT_COUNT; ch++) {
        hal.set(ch, on, hal.ctx);
    }
}

/* Every output rail (and so every mirrored LED) blinking together, fast, is
 * not a pattern any configured lighting/signal function produces — that's
 * what makes it "distinct" per AGENTS.md #3 on hardware with no dedicated
 * status LED. */
static void confirm_blink(void)
{
    for (int i = 0; i < BLINK_CYCLES; i++) {
        set_all_outputs(true);
        vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
        set_all_outputs(false);
        vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS));
        /* This now runs on the watchdog-monitored app task rather than during
         * boot, and the pattern takes ~1.4s — well past the TWDT timeout. Feed
         * it rather than let a confirmed factory reset look like a hang.
         * WITHOUT_ABORT because the app task is subscribed at this point in a
         * normal run but this file must not care: a failure to feed is not a
         * reason to abandon a reset the rider explicitly asked for. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_reset());
    }
}

static void erase_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) {
        return; /* namespace never existed (e.g. already-factory device) */
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}

/* Boot timestamp is not needed: the tick's own clock starts at boot, so
 * `now_ms` IS time-since-boot for this purpose. */
static bool s_armed;          /* window still open, or a press is in progress */
static bool s_press_active;   /* BOOT currently down and being timed */
static uint32_t s_press_start_ms;

void factory_reset_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    s_armed = true;
    s_press_active = false;
    s_press_start_ms = 0;

    ESP_LOGI(TAG, "factory reset armed: press BOOT within %us, then hold %us",
             FACTORY_RESET_WINDOW_MS / 1000u, FACTORY_RESET_HOLD_MS / 1000u);
}

/* Erases everything the outgoing owner put on the board, then reboots.
 *
 * "mc_calib" (board calibration) is deliberately NOT erased: it describes
 * this physical board's analog sense lines, not the outgoing owner's
 * identity or settings — the next owner keeps the same board and the same
 * correct calibration (see nvs_calib_hal.h). */
static void perform_reset(void)
{
    ESP_LOGW(TAG, "BOOT held %us: factory reset confirmed",
             FACTORY_RESET_HOLD_MS / 1000u);
    confirm_blink();

    erase_namespace("mc_cfg");
    erase_namespace("mc_keys");
    erase_namespace("mc_lock");

    /* Wipe the event log's own history too, then log the wipe itself as the
     * sole surviving record — same doctrine as MC_OP_TRANSFER_OWNERSHIP's
     * identical wipe-then-log-the-wipe in mc_session.c
     * (docs/PROTOCOL.md §15). */
    if (nvs_event_log_hal_erase() == ESP_OK && nvs_event_log_hal_init() == ESP_OK) {
        mc_event_log_t log;
        mc_event_log_init(&log, nvs_event_log_hal_get());
        mc_event_log_append(&log, MC_EVT_FACTORY_RESET, 0, 0, 0);
    } else {
        ESP_LOGW(TAG, "event log erase/reinit failed during factory reset");
    }

    ESP_LOGW(TAG, "factory reset complete: config, keys, lock state and event "
                  "log wiped; restarting");
    /* Restart rather than carry on: mc_config/mc_keystore/mc_lock are already
     * loaded into RAM by this point in the run, and continuing would keep
     * serving exactly the state that was just erased until the next power
     * cycle. A reboot re-reads the now-empty NVS and comes up factory-fresh,
     * which is also what makes the outputs settle to defaults. */
    esp_restart();
}

void factory_reset_tick(uint32_t now_ms)
{
    if (!s_armed) {
        return; /* window closed — inert for the rest of this run */
    }

    /* BOOT/SW2 is active-low: pressed pulls the pin to GND. */
    bool down = (gpio_get_level(BOARD_BOOT_PIN) == 0);

    if (!s_press_active) {
        if (now_ms >= FACTORY_RESET_WINDOW_MS) {
            /* Nothing in progress and the window has expired. Disarm for
             * good, so no later button fault can trigger a wipe on a moving
             * bike. */
            s_armed = false;
            return;
        }
        if (down) {
            s_press_active = true;
            s_press_start_ms = now_ms;
            ESP_LOGW(TAG, "BOOT pressed; hold %us to factory reset",
                     FACTORY_RESET_HOLD_MS / 1000u);
        }
        return;
    }

    if (!down) {
        ESP_LOGI(TAG, "BOOT released after %ums; factory reset cancelled",
                 (unsigned)(now_ms - s_press_start_ms));
        s_press_active = false;
        return;
    }

    if (now_ms - s_press_start_ms >= FACTORY_RESET_HOLD_MS) {
        perform_reset(); /* does not return */
    }
}
