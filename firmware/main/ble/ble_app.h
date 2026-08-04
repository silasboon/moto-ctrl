#pragma once

#include "mc_session.h"

/* Brings up the NimBLE host: registers the MOTO-CTRL GATT services, enables
 * LE Secure Connections bonding, and starts connectable advertising under
 * the board's configured name (or "MOTO-CTRL" if the rider hasn't set one).
 * `app` is the shared device state the sessions operate on and must outlive
 * the BLE stack. Call once, after NVS is initialized. */
void ble_app_start(mc_app_t *app);

/* Re-reads the board name from the config and republishes it: GAP device
 * name plus a rebuilt advertisement, since the name lives in the
 * scan-response payload. Wire this to mc_app_t.on_device_name_changed. */
void ble_app_refresh_device_name(void);
