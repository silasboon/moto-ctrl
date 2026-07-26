#pragma once

#include "mc_session.h"

/* Brings up the NimBLE host: registers the MOTO-CTRL GATT services, enables
 * LE Secure Connections bonding, and starts connectable advertising as
 * "MOTO-CTRL". `app` is the shared device state the sessions operate on and
 * must outlive the BLE stack. Call once, after NVS is initialized. */
void ble_app_start(mc_app_t *app);
