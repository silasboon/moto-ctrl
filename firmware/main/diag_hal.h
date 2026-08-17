#pragma once

#include "mc_diag.h"

/* Real DSEL/DEN-sequenced ADC read for the shared PROFET current-sense (IS)
 * line and the battery-voltage divider (hardware/PINOUT.md "Diagnostics
 * readout procedure"). Call once, after board_config_early_init() (GPIO46 —
 * U4's DEN line — is a strapping pin; never configure it with an internal
 * pullup, per PINOUT.md's warning and board_config_early_init()'s own) and
 * before the app task starts ticking mc_diag.
 *
 * No host test covers this file: unlike firmware/components/core/mc_diag.c,
 * which the simulator compiles and tests, the DSEL/DEN mux sequencing and
 * ADC reads here can only be exercised on a real board. Re-check it on
 * hardware after changes — see docs/HARDWARE_TESTING.md. */
void diag_hal_init(void);

mc_diag_hal_t diag_hal_get(void);
