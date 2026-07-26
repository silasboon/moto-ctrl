#include "factory_reset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"

#include "mc_event_log.h"

#include "board_config.h"
#include "nvs_event_log_hal.h"
#include "output_hal_gpio.h"

static const char *TAG = "mc_factory_reset";

#define HOLD_MS 10000u
#define POLL_MS 50u
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

bool factory_reset_check(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* BOOT/SW2 is active-low: pressed pulls the pin to GND. */
    if (gpio_get_level(BOARD_BOOT_PIN) != 0) {
        return false;
    }

    ESP_LOGW(TAG, "BOOT held at power-on; hold for %u s to factory reset", HOLD_MS / 1000u);

    uint32_t held_ms = 0;
    while (held_ms < HOLD_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (gpio_get_level(BOARD_BOOT_PIN) != 0) {
            ESP_LOGI(TAG, "BOOT released after %u ms; factory reset cancelled", held_ms);
            return false;
        }
        held_ms += POLL_MS;
    }

    ESP_LOGW(TAG, "BOOT held %u s: factory reset confirmed", HOLD_MS / 1000u);
    confirm_blink();

    erase_namespace("mc_cfg");
    erase_namespace("mc_keys");
    erase_namespace("mc_lock");
    /* "mc_calib" (board calibration) is deliberately NOT erased
     * here: it describes this physical board's analog sense lines, not the
     * outgoing owner's identity/settings — the next owner keeps the same
     * board and the same correct calibration (see nvs_calib_hal.h). */

    /* Wipe the event log's own history too, then log the wipe
     * itself as the sole surviving record — same doctrine as
     * MC_OP_TRANSFER_OWNERSHIP's identical wipe-then-log-the-wipe in
     * mc_session.c (docs/PROTOCOL.md §15). Runs on its own dedicated
     * "evtlog" NVS partition, which nothing else has opened yet at this
     * point in boot (app_main() calls this before initializing the
     * long-lived event log instance it uses for the rest of the run). */
    if (nvs_event_log_hal_erase() == ESP_OK && nvs_event_log_hal_init() == ESP_OK) {
        mc_event_log_t log;
        mc_event_log_init(&log, nvs_event_log_hal_get());
        mc_event_log_append(&log, MC_EVT_FACTORY_RESET, 0, 0, 0);
    } else {
        ESP_LOGW(TAG, "event log erase/reinit failed during factory reset");
    }

    ESP_LOGW(TAG, "factory reset complete: config, keys, lock state, and event log wiped");
    return true;
}
