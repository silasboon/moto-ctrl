# app/

MOTO-CTRL companion app — bare React Native + TypeScript (no Expo), offline
first, zero cloud dependency, no accounts (see
[`CONTRIBUTING.md`](../CONTRIBUTING.md) for the full rule and its one
narrow exception). BLE/key logic must go through the `Transport` interface.

## Layout

- `src/transport/` — the `Transport` interface (channel-aware: `send`/
  `onMessage` take the logical channel explicitly, since BLE and the
  WebSocket sim frame it differently — see `docs/PROTOCOL.md` §1) and its
  two implementations:
  - `SimTransport` — talks to `firmware/sim/` over WebSocket. Used for all
    local dev and CI, so neither needs a board attached.
  - `BlePlxTransport` — real hardware, via `react-native-ble-plx`. Verified
    against a v1 board; re-check it per release with
    `docs/HARDWARE_TESTING.md`.
- `src/protocol/` — the real wire protocol (`docs/PROTOCOL.md`), transport-
  agnostic: `constants.ts` (opcodes/channels, mirrors `mc_protocol.h` by
  hand — deliberately excludes the simulator-only debug channel), `frames.ts`
  (byte/base64/utf8 helpers), `types.ts` (status + config JSON shapes), and
  `MotoClient.ts` — the high-level async client (auth, status polling,
  commands, config read/write) everything else is built on.
- `src/identity/KeyStore.ts` — persists this phone's Ed25519 identity
  (AsyncStorage, not platform secure storage — see the file's own header
  comment for why and `docs/TESTING.md` for when that might change) and the
  last-paired device.
- `src/update/updateCheck.ts` — the app's one permitted network call
  (fetching/verifying a firmware update manifest and bundle — see
  `docs/PROTOCOL.md` §10.5 and `CONTRIBUTING.md`'s "No cloud, no telemetry,
  no accounts" section), kept in its own module deliberately separate from
  `src/protocol/` (BLE/simulator wire protocol only).
- `src/screens/` — `PairingScreen` finds and authenticates a board.
  `DashboardScreen` ("Ride") is the landing screen: status, output control,
  a quick lock/unlock action, a battery/fault/low-voltage-cutoff indicator,
  and a Hazard button. Everything else sits one tap away behind
  `SettingsScreen`, the hub reached from Ride's settings button:
  - `OutputsScreen` — name each of the 12 channels, pick its behaviour
    (toggle, momentary, blink, flasher) and PWM dimming, and set the role
    flags that carry real firmware behaviour (`essential`, `is_ignition`,
    `is_starter`, `is_brake`).
  - `ButtonsScreen` — name the 8 inputs and bind single/double/hold presses
    and chords to output channels or turn/hazard actions, with an
    identify-by-pressing flow (`INPUT_LEARN`).
  - `BoardScreen` — settings about the board itself (currently its name).
  - `KeysScreen`, `LockScreen` (immobilizer enable/methods, cheat-code
    set/clear/test, ownership transfer).
  - `DiagnosticsScreen` — live per-channel current + fault, learnable
    open-load/overcurrent thresholds, low-voltage-cutoff and
    engine-running voltage config, board calibration.
  - `FirmwareUpdateScreen` (checks for/downloads/transfers a signed OTA
    update, then applies it on request), `EventLogScreen` (read-only viewer
    for the device's persisted security/safety event log), and
    `BoardInfoCard` (firmware version + uptime, shown atop Settings).

  Screens are switched via local state in `App.tsx` — no navigation library
  (see `App.tsx`'s header comment for why). Leaving a screen with unsaved
  edits routes through `src/ui/NavGuard.tsx`, so an edge-swipe back gets the
  same confirmation the screen's own Back chevron does.
- `ios/`, `android/` — the committed bare-RN native projects (bundle ID
  `com.motoctrl.app`, BLE permissions configured). See
  [`NATIVE_SETUP.md`](NATIVE_SETUP.md) to build or regenerate them.

## Status

Pairing, dashboard, output and button configuration, the lock/immobilizer
screen, diagnostics, turn-signal/hazard/brake-flasher/PWM control, firmware
updates, and the event log viewer are all implemented against
`SimTransport` and verified against a real `firmware/sim/` instance (see
Testing below), and `BlePlxTransport` has been exercised against a real
board over BLE.

The cheat-code is write-only from the app's perspective by design — the
device only ever reports whether one is set and how long it is (never the
sequence, never a hash); physical entry happens on the handlebar buttons,
not through this app.

Diagnostics thresholds/cutoff config go through dedicated `DIAG_GET_CONFIG`/
`DIAG_SET_CONFIG` wire ops (same pattern as lock config), not the generic
config JSON, even though they also ride it for export/import completeness
(`DeviceConfig.diagnostics`) — `DiagnosticsScreen` edits live values through
the dedicated ops. Board calibration is a separate, even-less-often-touched
set of ops (`DIAG_GET_CALIB`/`DIAG_SET_CALIB`) that never rides a config
export/import or gets cleared by ownership transfer, since it describes the
physical board, not the owner.

Flasher/PWM config (behaviour, duty, auto-cancel/blink/pulse timing,
brake-switch input) is different from both of the above: it rides the
generic config JSON exclusively (`OutputsScreen`, alongside name and role
flags — output config never had its own dedicated wire channel, unlike
lock/diagnostics). The one dedicated opcode here is `hazardPress()` — a
plain turn-signal toggle is still just `setOutput()` on a channel whose
`indicator` is set to left or right; mutual exclusion and the auto-cancel
timer are applied device-side, not by this app.

Known MVP simplifications, not gaps in a safety requirement:

- Enrolling a second phone is done by pasting its base64 public key (shown
  on its own Pairing screen) into the first phone's Keys screen — no QR/
  camera flow, to avoid pulling in a camera dependency for a one-off step.
- The phone's private key lives in AsyncStorage, not the iOS Keychain /
  Android Keystore — a deliberate initial simplification, not yet
  revisited.
- `MotoClient` polls status on an interval rather than relying on
  device-pushed notifications: real firmware only replies to an explicit
  `STATUS_GET` today (unlike the simulator's dev-convenience ticker).
- **Background BLE reconnect is not implemented.** `BlePlxTransport`
  constructs `BleManager` with no `restoreStateIdentifier`, there is no
  `bluetooth-central` entry in `UIBackgroundModes`, and Android has no
  foreground service. Phone-as-key auto-unlock therefore needs the app in
  the foreground; it will not fire with the phone locked in a pocket. This
  is the one gap that is load-bearing for the immobilizer, which is why a
  non-phone unlock method is mandatory before it can be enabled.

## Development

```sh
npm install
npm run typecheck
npm run lint
npm test
```

`npm test` includes a live integration suite
(`src/__tests__/sim.itest.test.js`) that spawns a real `firmware/sim/`
instance and drives it through `SimTransport` + `MotoClient` — build the sim
first, or it skips gracefully:

```sh
cd ../firmware/sim && cmake -S . -B build && cmake --build build
```

Running on a device (`npm run ios` / `npm run android`) needs Xcode or the
Android SDK — see [`NATIVE_SETUP.md`](NATIVE_SETUP.md). BLE needs a physical
device; it does not work on the iOS Simulator or a stock Android emulator.
