# firmware/

ESP-IDF (C, FreeRTOS, NimBLE) firmware for the MOTO-CTRL board
(ESP32-S3-WROOM-1-N4). See [`CONTRIBUTING.md`](../CONTRIBUTING.md) for the
safety requirements and pin-config rule that govern everything in this
directory, and [`hardware/PINOUT.md`](../hardware/PINOUT.md) for the pin
contract.

## Layout

- `components/core/` — the portable core: state machines + protocol + auth,
  plain C99 with no ESP-IDF/FreeRTOS dependency, built by both the on-target
  component and `sim/` from the same sources.
  - `mc_output` (12-channel output engine; on/off, PWM dimming, and
    turn/brake flasher patterns — see below), `mc_input`
    (debounce, short/long/double press, chord + sequence combos),
    `mc_config` (versioned config blob + migration), `mc_persist`
    (debounced-write scheduler for NVS wear).
  - `mc_crypto` (Ed25519 verify/sign + CSPRNG), `mc_keystore` (enrolled
    public keys), `mc_status` (status snapshot), `mc_config_json`
    (config ↔ JSON), `mc_session` (the transport-agnostic protocol + auth
    engine), `mc_protocol.h` (wire constants).
  - `vendor/` — TweetNaCl (Ed25519, public domain) + cJSON (MIT), unmodified.
- `components/board_config/` — the single source of GPIO pin definitions,
  generated from `hardware/PINOUT.md`, plus `board_config_early_init()`
  (GPIO3/GPIO46 strapping-pin safety). No other file may hardcode a GPIO
  number.
- `main/` — `app_main` and the ESP-IDF-specific glue: GPIO HALs
  (`output_hal_gpio` — plain digital plus lazily-attached LEDC PWM for
  opt-in dimming; `input_hal_gpio`), the current-sense/battery ADC HAL
  (`diag_hal` — DSEL/DEN mux sequencing, `hardware/PINOUT.md`), NVS stores
  (`nvs_config_hal`, `nvs_keystore_hal`,
  `nvs_lock_hal`, `nvs_calib_hal`), the physical factory-reset flow
  (`factory_reset.c`, BOOT-hold), `watchdog`, and `ble/` — the NimBLE GATT
  server (`gatt_svr.c`), LE Secure Connections bonding + advertising
  (`ble_app.c`), and UUIDs (`ble_uuids.h`), all wiring the BLE transport to
  `mc_session`.
- `sim/` — a standalone host (Linux/macOS) CMake project that builds
  `components/core/` behind a self-contained WebSocket "fake BLE" server
  (`src/ws_server.c`), plus C unit tests (`sim/tests/`) and Node integration
  tests (`sim/itest/`), so the app and CI run with no hardware. Independent
  of the ESP-IDF build.
  - `sim/src/sim_debug.c` + `sim_nvs.c` + `sim_protocol.h` — the sim-only
    debug/fault-injection channel (fake battery/current/fault, forced
    disconnect/reboot/NVS-corruption, event log) and fake NVS blob store
    that back it. Never part of the real protocol — see `docs/TESTING.md`.
  - `sim/gui/` — a static browser debug console (`index.html`/`app.js`, no
    build step) that drives the sim over WebSocket: live output state +
    real per-channel current/fault, a Hazard button, virtual buttons,
    auth/key management, lock/immobilizer config, diagnostics
    thresholds/calibration, config read/write (mode/duty/flasher-timing
    editing lives here, not as dedicated Outputs-panel widgets — see
    docs/TESTING.md), OTA transfer controls, an event log viewer, and
    scenario record/replay. The primary day-to-day dev tool for the app and
    firmware alike.
- `partitions.csv` / `sdkconfig.defaults` — 4MB flash, OTA A/B partitions
  with rollback, NimBLE-only Bluetooth (bonds persisted to NVS), no PSRAM,
  no WiFi.

## Building the firmware (on-target)

Requires the [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
toolchain (latest stable), targeting `esp32s3`:

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3
idf.py build
```

Flashing uses UART0 only (3-pin TX/RX/GND header, no USB, no auto-reset) —
see [`docs/FLASHING.md`](../docs/FLASHING.md) for the manual BOOT/EN sequence.

## Building the host simulator

No ESP-IDF toolchain required — just CMake and a C compiler:

```sh
cd firmware/sim
cmake -S . -B build
cmake --build build
ctest --test-dir build            # C unit tests

# Integration tests (WebSocket protocol + C<->JS Ed25519 interop) need Node:
cd itest && npm ci && MOTO_SIM_BIN=../build/moto_ctrl_sim npm test
```

Run the simulator standalone for app/GUI development (default port 8010):

```sh
./firmware/sim/build/moto_ctrl_sim 8010   # ws://127.0.0.1:8010
```

Then open `firmware/sim/gui/index.html` in a browser and click Connect. See
[`docs/TESTING.md`](../docs/TESTING.md) for the full harness (debug
protocol, scenario record/replay, QEMU boot validation).

## Building under QEMU (boot validation only — not BLE)

```sh
. $IDF_PATH/export.sh
cd firmware
idf.py set-target esp32s3 && idf.py build
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
timeout 30 idf.py qemu
```

Validates boot sequence, NVS/config load, output restore, and watchdog init
on the real binary. Does **not** validate BLE — see
[`docs/TESTING.md`](../docs/TESTING.md) §5 before drawing any conclusion
about pairing/bonding/auth from a QEMU run.

## Status

The full feature set is implemented and tested:

- **BLE stack.** The transport-agnostic protocol + auth engine
  (`mc_session`): status, control, and config (chunked JSON) channels, with
  all state-changing operations gated behind an Ed25519 challenge-response
  (`mc_crypto` + `mc_keystore`). The device stores only public keys; nonces
  are single-use. NimBLE GATT server with LE Secure Connections bonding
  (bonds in NVS), advertising as `MOTO-CTRL`, per-connection sessions.
- **Pre-hardware validation harness.** A real `mc_input` engine runs inside
  the sim (10ms tick); a fake NVS blob store makes config/keystore/lock/
  calibration persistence — and its corruption — real and reloadable via a
  simulated reboot; the sim-only debug channel + browser GUI (`sim/gui/`);
  and QEMU boot validation, which boots the real cross-compiled binary and
  checks its boot-sequence log markers. QEMU is a local-only step — it is
  **not** part of CI, and it does not and cannot validate BLE. See
  `docs/TESTING.md` §5 before drawing conclusions from it.
- **Lock / immobilizer** (`mc_lock`, `docs/PROTOCOL.md` §11): the
  four-state FSM (DISABLED/UNLOCKED/PARKED/LOCKED), edge-triggered
  phone-as-key auto-unlock, a salted-hash button cheat-code with
  progressive wrong-entry backoff, ignition-switch mode, ownership
  transfer, and the physical BOOT-hold factory reset (`factory_reset.c`).
- **Diagnostics** (`mc_diag`, `docs/PROTOCOL.md` §12): round-robin
  per-channel current sensing over the shared PROFET IS line
  (`hardware/PINOUT.md`) with learnable open-load/overcurrent thresholds;
  real voltage-derived `engine_running`, feeding the starter-protection and
  lock parked-detection guards; a low-voltage battery cutoff that
  suppresses non-essential outputs (`CONTRIBUTING.md` safety requirement
  #7) while leaving commanded/intent state untouched; and board calibration
  in its own NVS blob, excluded from config export/import and from factory
  reset (it describes the board, not the owner).
- **Flashers / PWM** (`mc_output`, `docs/PROTOCOL.md` §13): turn signals
  and hazard, with mutual exclusion and an auto-cancel timer embedded
  directly in `mc_output_set()` (so any caller — a handlebar button, or the
  `HAZARD_PRESS` wire op — gets correct behavior for free), a brake-flasher
  attention-pulse burst (opt-in, off by default per `CONTRIBUTING.md`
  safety requirement #5), and opt-in per-channel PWM dimming
  (lazily-attached LEDC on real hardware). Handlebar buttons bind to
  actions besides the cheat-code — each of `short_press_action[]`,
  `long_press_action[]` and `double_press_action[]` holds a per-button LIST
  of actions (turn-L/turn-R toggle, hazard press, or `256 + N` to toggle
  output channel N directly) — and a physical brake-lever/pedal switch input
  (`brake_switch_input`, read as a level, mirrors
  `starter_interlock_input`).
- **OTA + config migration + event log** (`mc_ota`, `mc_event_log`,
  `docs/PROTOCOL.md` §10/§15): signed firmware updates over BLE (A/B
  partitions, Ed25519 signature over a streamed SHA-512 digest so no PSRAM
  buffering is needed, safe-state gating on `engine_running`/low-voltage
  cutoff at both begin and reboot time) and a persisted, fixed-size ring
  buffer of security/safety events (lock transitions, key enroll/revoke/
  transfer, factory reset, cheat-code lockout, OTA begin/success/failure,
  low-voltage cutoff), readable over its own chunked wire op. Config
  migration needed no new code: `mc_config_json`'s already-tolerant parsing
  (missing fields default, unknown fields ignored) handles an older or
  newer-but-understood document without a separate binary migration path —
  only a future schema version this firmware doesn't understand is
  rejected outright. `tools/sign-firmware.py` generates the release keypair
  and signs images into `.mcota` bundles; the app has a Firmware Update
  screen and an Event Log viewer.
- Protocol spec: [`docs/PROTOCOL.md`](../docs/PROTOCOL.md).

Verified: host `ctest` (12 suites, including `test_ota.c`/
`test_event_log.c`), the Node integration suite (33 cases against a real
spawned sim, including a full signed OTA transfer, safe-state-gating
rejection, and event log read/since_seq coverage), and the app's own Jest
suite (116 cases across 11 files) — all pass. Beyond that, the firmware has
been built for the real `esp32s3` target, flashed to a board, and driven
over BLE from the app, and the bench checklist in
[`../docs/HARDWARE_TESTING.md`](../docs/HARDWARE_TESTING.md) has been worked
through against real hardware. Re-run that checklist per release rather than
treating it as settled — it is the only layer that covers real GPIO, real
current sensing, and real BLE.

Two implementation notes worth knowing about:

- `firmware/sdkconfig.defaults` enables
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, so every OTA-updated image boots
  in "pending verify" state and the bootloader would revert on the *next*
  reset unless the image confirms itself. `app_main()` calls
  `esp_ota_mark_app_valid_cancel_rollback()` as its last step, once every
  boot stage (config/output/lock restore, diagnostics, input, watchdog,
  BLE) has completed without crashing — see the comment at that call site,
  and `docs/HARDWARE_TESTING.md` §11 for the reset-after-OTA check.
- The physical factory reset is a 5-second arming window watched on the app
  tick (`factory_reset_init()` / `factory_reset_tick()`), not a sample at
  boot: holding BOOT through reset enters UART download mode, because BOOT
  is GPIO0 and its level at the strap instant selects the boot path, so
  `app_main()` would never run. The gesture is "apply power, then press and
  hold BOOT" — see `firmware/main/factory_reset.h`, `docs/FLASHING.md`'s
  Factory reset section, and `docs/HARDWARE_TESTING.md` §9.

The generic `combos[]` chord/sequence mechanism dispatches its own
`actions` list, orthogonal to the lock cheat-code and to the per-button
press bindings, which all consume short-press events independently of the
combo matcher. A matched *chord* additionally marks its member buttons so
their own bindings don't also fire (the press event is still delivered,
flagged `action_suppressed`, so the cheat-code can never be starved — see
`mc_input.h`). A matched *sequence* suppresses nothing, since its member
presses were already delivered before it completed.
