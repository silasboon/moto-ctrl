#pragma once

#include "mc_session.h"

/* Registers the MOTO-CTRL GATT services (status / control / config / OTA)
 * and binds them to the shared app state. Call after nimble_port_init() and
 * the standard GATT/GAP service init, before ble_gatts_start(). */
int gatt_svr_init(mc_app_t *app);

/* Session lifecycle, driven by the GAP connect/disconnect events. */
void gatt_svr_on_connect(uint16_t conn_handle);
void gatt_svr_on_disconnect(uint16_t conn_handle);
