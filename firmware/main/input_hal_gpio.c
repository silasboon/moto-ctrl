#include "input_hal_gpio.h"

#include "driver/gpio.h"

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
