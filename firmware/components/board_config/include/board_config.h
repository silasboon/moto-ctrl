#pragma once

/*
 * board_config — the ONE place GPIO numbers are allowed to appear in
 * firmware. Generated from hardware/PINOUT.md (board "Integrated V2",
 * schematic 2026-07-18). If PINOUT.md changes for a new hardware revision,
 * regenerate this header from it — do not hand-edit pins in both places.
 *
 * Channels are 0-indexed here (channel 0 == OUT1 .. channel 11 == OUT12),
 * matching mc_output's 0-indexed channel numbering.
 */

#include <stdint.h>

#define BOARD_CONFIG_NAME "moto-ctrl-v2"

#define BOARD_OUTPUT_COUNT 12
#define BOARD_INPUT_COUNT 8

/* --- PROFET outputs: IN pin per channel (OUT1..OUT12), 0-indexed --- */
#define BOARD_OUTPUT_IN_PINS \
    { 47, 14, 13, 11, 10, 3, 8, 17, 16, 7, 6, 4 }

/* DEN pin per channel — each PROFET's DEN line is shared by its two
 * channels (U2 DEN1 for OUT1/OUT2, U3 DEN2 for OUT3/OUT4, etc). Only one
 * PROFET may have DEN high at a time (see PINOUT.md's diagnostics
 * readout procedure) — that round-robin sequencing is handled in mc_diag,
 * this table just records which DEN pin belongs to which channel. */
#define BOARD_OUTPUT_DEN_PINS \
    { 21, 21, 12, 12, 46, 46, 18, 18, 15, 15, 5, 5 }

#define BOARD_DSEL_PIN 48
#define BOARD_IS_ADC_GPIO 9 /* ADC1_CH8 */

/* --- Handlebar buttons BTN1..BTN8, 0-indexed, active-low at the pin --- */
#define BOARD_BUTTON_PINS \
    { 35, 36, 37, 38, 39, 40, 41, 42 }

/* --- Analog --- */
#define BOARD_VSENSE_BAT_ADC_GPIO 1 /* ADC1_CH0 */
#define BOARD_VSENSE_BAT_DIVIDER_RATIO 0.0909f /* 1MΩ/100kΩ, calibrate offset/gain per board */

/* --- System / programming --- */
#define BOARD_UART_TX_PIN 43
#define BOARD_UART_RX_PIN 44
#define BOARD_BOOT_PIN 0 /* SW2: also the runtime factory-reset button (hold at power-on) */

/* --- Reserved --- */
#define BOARD_SPARE_ADC_GPIO 2 /* future NTC board-temp / 5V rail monitor; driver stub only */

/*
 * Strapping pins used as PROFET outputs: GPIO3 (OUT6 IN, U4 PROFET_IN6)
 * and GPIO46 (U4 PROFET_DEN3, shared by OUT5/OUT6). Verified safe on this
 * hardware (see PINOUT.md), but firmware must never enable internal
 * pullups on these two pins before boot completes, and must drive them
 * low during early init — do this before any other GPIO or peripheral
 * setup. See board_config_early_init() in board_config.c.
 */
#define BOARD_STRAP_PIN_OUT6_IN 3
#define BOARD_STRAP_PIN_U4_DEN 46

/* Drives the GPIO3/GPIO46 strapping pins low with no pullups, before any
 * other GPIO or peripheral init. Call this first thing in app_main(),
 * before mc_output_restore_from_config() or anything else touches GPIO. */
void board_config_early_init(void);
