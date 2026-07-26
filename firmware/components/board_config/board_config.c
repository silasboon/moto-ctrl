#include "board_config.h"

#include "driver/gpio.h"

void board_config_early_init(void)
{
    /* GPIO3/GPIO46 are ESP32-S3 strapping pins wired as PROFET IN6/DEN3
     * outputs on this board. Verified safe by the hardware design (see
     * PINOUT.md), but they must be driven low with no pullups before
     * anything else touches GPIO, per that same warning. This function
     * must be the first thing app_main() calls. */
    const gpio_num_t strap_pins[] = { BOARD_STRAP_PIN_OUT6_IN, BOARD_STRAP_PIN_U4_DEN };

    for (size_t i = 0; i < sizeof(strap_pins) / sizeof(strap_pins[0]); i++) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << strap_pins[i],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);
        gpio_set_level(strap_pins[i], 0);
    }
}
