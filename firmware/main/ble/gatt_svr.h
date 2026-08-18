#pragma once

#include "mc_session.h"

/* Registers the MOTO-CTRL GATT services (status / control / config / OTA)
 * and binds them to the shared app state. Call after nimble_port_init() and
 * the standard GATT/GAP service init, before ble_gatts_start(). */
int gatt_svr_init(mc_app_t *app);

/* Session lifecycle, driven by the GAP connect/disconnect events. */
void gatt_svr_on_connect(uint16_t conn_handle);
void gatt_svr_on_disconnect(uint16_t conn_handle);

/* Pushes one input event (MC_OP_INPUT_EVENT) to every session that has
 * MC_OP_INPUT_LEARN enabled. A no-op when no session is in learn mode, which
 * is the normal riding case — so calling this for every press is cheap.
 *
 * Called from app_task, not the NimBLE host task, so it must not be given
 * anything that isn't safe to touch from another task. It only reads the
 * session table and calls ble_gatts_notify_custom(), which is safe to invoke
 * from an application task. */
void gatt_svr_push_input_event(uint8_t button, uint8_t press_type, bool action_suppressed);

/* True while any authenticated session has asked for learn mode WITH action
 * suppression (docs/PROTOCOL.md §14.1) — the app is capturing a cheat-code,
 * and the buttons being pressed must not also fire whatever they are bound
 * to. The caller skips its binding dispatch while this holds. */
bool gatt_svr_input_actions_suppressed(void);

/* How many BLE clients are currently attached. mc_power keeps the loop out
 * of its parked profile while anyone is connected — slow advertising does
 * nothing for an established link, and the slowest tick would just make the
 * app feel laggy while it is in use. */
uint8_t gatt_svr_connection_count(void);
