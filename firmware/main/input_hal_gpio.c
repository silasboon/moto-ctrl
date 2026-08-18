#include "input_hal_gpio.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"

static const gpio_num_t s_btn_pins[BOARD_INPUT_COUNT] = BOARD_BUTTON_PINS;

void input_hal_gpio_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BOARD_INPUT_COUNT; i++) {
        mask |= (1ULL << s_btn_pins[i]);
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

void input_hal_gpio_sample(bool raw_pressed[BOARD_INPUT_COUNT])
{
    for (int i = 0; i < BOARD_INPUT_COUNT; i++) {
        raw_pressed[i] = (gpio_get_level(s_btn_pins[i]) == 0); /* active-low */
    }
}

/* --- Parked wake path (see input_hal_gpio.h) --- */

static TaskHandle_t s_wake_task;
static portMUX_TYPE s_wake_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_masked; /* bit i: button i's interrupt is masked, guarded by s_wake_mux */

/* Light-sleep GPIO wake is level-triggered — gpio_wakeup_enable() rejects
 * edge modes outright, and it OVERWRITES the pin's interrupt type to do it.
 * So this runs as a low-level interrupt, which stays asserted for as long as
 * the button is down and would re-enter forever if it were not masked here.
 *
 * That is not a theoretical worst case: an ignition switch wired to an input
 * is a maintained switch, held to ground for the whole time the key is on,
 * so an unmasked handler is an immediate interrupt-watchdog panic rather
 * than a rare race. The mask is lifted by input_hal_gpio_wake_rearm() once
 * the pin reads released again.
 *
 * gpio_intr_disable() is a bare register write with no lock of its own,
 * which is what makes it safe to call from here. */
static void button_isr(void *arg)
{
    uint32_t idx = (uint32_t)(uintptr_t)arg;

    gpio_intr_disable(s_btn_pins[idx]);
    portENTER_CRITICAL_ISR(&s_wake_mux);
    s_masked |= (1u << idx);
    portEXIT_CRITICAL_ISR(&s_wake_mux);

    if (s_wake_task != NULL) {
        BaseType_t higher_woken = pdFALSE;
        vTaskNotifyGiveFromISR(s_wake_task, &higher_woken);
        portYIELD_FROM_ISR(higher_woken);
    }
}

void input_hal_gpio_wake_rearm(void)
{
    uint32_t masked;
    portENTER_CRITICAL(&s_wake_mux);
    masked = s_masked;
    portEXIT_CRITICAL(&s_wake_mux);

    if (masked == 0) {
        return;
    }

    /* Re-enable outside the critical section: gpio_intr_enable() takes the
     * driver's own spinlock, and nesting the two would invite a lock-order
     * problem for no benefit. */
    uint32_t rearmed = 0;
    for (int i = 0; i < BOARD_INPUT_COUNT; i++) {
        if ((masked & (1u << i)) == 0) {
            continue;
        }
        if (gpio_get_level(s_btn_pins[i]) == 0) {
            continue; /* still held — leave it masked */
        }
        gpio_intr_enable(s_btn_pins[i]);
        rearmed |= (1u << i);
    }

    if (rearmed != 0) {
        portENTER_CRITICAL(&s_wake_mux);
        s_masked &= ~rearmed;
        portEXIT_CRITICAL(&s_wake_mux);
    }
}

void input_hal_gpio_wake_init(TaskHandle_t task_to_notify)
{
    s_wake_task = task_to_notify;

    /* Order matters: set the wake type first (this is what makes the pins
     * low-level triggered), then attach handlers. gpio_isr_handler_add() is
     * what actually enables the CPU interrupt, so any pin already held low
     * — an ignition key left on across a reboot — fires once at that point
     * and immediately masks itself. Self-correcting, and exactly one entry. */
    for (int i = 0; i < BOARD_INPUT_COUNT; i++) {
        esp_err_t err = gpio_wakeup_enable(s_btn_pins[i], GPIO_INTR_LOW_LEVEL);
        if (err != ESP_OK) {
            ESP_LOGE("mc_input_hal", "gpio_wakeup_enable(%d) failed (%s)",
                     (int)s_btn_pins[i], esp_err_to_name(err));
        }
    }
    esp_sleep_enable_gpio_wakeup();

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already installed is fine */
        ESP_LOGE("mc_input_hal", "gpio_install_isr_service failed (%s); "
                                 "parked button wake falls back to polling",
                 esp_err_to_name(err));
        return;
    }

    for (int i = 0; i < BOARD_INPUT_COUNT; i++) {
        gpio_isr_handler_add(s_btn_pins[i], button_isr, (void *)(uintptr_t)i);
    }
}
