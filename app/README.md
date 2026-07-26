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
    local dev and CI, since no board exists yet.
  - `BlePlxTransport` — real hardware, via `react-native-ble-plx`.
    **Unverified**: there's no board and no generated native project to run
    it against (see `NATIVE_SETUP.md`) — needs bench verification once
    hardware exists (`docs/HARDWARE_TESTING.md`).
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
- `src/screens/` — `PairingScreen`, `DashboardScreen` (status + output
  control together, plus a quick lock/unlock action, a battery/fault/
  low-voltage-cutoff indicator, and a Hazard button), `PinMapperScreen`
  (also edits each channel's mode — on/off, PWM dimming, turn-signal blink,
  brake flasher — plus the brake-switch input and flasher timing settings),
  `KeysScreen`, `LockScreen` (immobilizer enable/methods, cheat-code
  set/clear/test, ownership transfer), `DiagnosticsScreen` (live
  per-channel current + fault, learnable open-load/overcurrent thresholds,
  low-voltage-cutoff and engine-running voltage config, board calibration),
  `FirmwareUpdateScreen` (checks for/downloads/transfers a signed OTA
  update, then applies it on request), and `EventLogScreen` (read-only
  viewer for the device's persisted security/safety event log). Switched
  via local state in `App.tsx`, no navigation library (see `App.tsx`'s
  header comment for why).
- `ios/`, `android/` — **not committed yet**, see
  [`NATIVE_SETUP.md`](NATIVE_SETUP.md).

## Status

Pairing, dashboard, pin mapper, output control, the lock/immobilizer
screen, diagnostics, turn-signal/hazard/brake-flasher/PWM control, firmware
updates, and the event log viewer are all implemented against
`SimTransport` and verified against a real `firmware/sim/` instance (see
Testing below). `BlePlxTransport` is written but unverified — no hardware
exists yet.

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

Flasher/PWM config (mode, duty, auto-cancel/blink/pulse timing,
brake-switch input) is different from both of the above: it rides the
generic config JSON exclusively (`PinMapperScreen`, alongside
function/name — output config never had its own dedicated wire channel to
begin with, unlike lock/diagnostics). The one dedicated opcode here is
`hazardPress()` — a plain turn-signal toggle is still just `setOutput()`
on the `turn_l`/`turn_r`-function channel; mutual exclusion and the
auto-cancel timer are applied device-side, not by this app.

Known MVP simplifications, not gaps in a safety requirement:

- Enrolling a second phone is done by pasting its base64 public key (shown
  on its own Pairing screen) into the first phone's Keys screen — no QR/
  camera flow, to avoid another native dependency this environment can't
  verify.
- The phone's private key lives in AsyncStorage, not the iOS Keychain /
  Android Keystore — a deliberate initial simplification, not yet
  revisited.
- `MotoClient` polls status on an interval rather than relying on
  device-pushed notifications: real firmware only replies to an explicit
  `STATUS_GET` today (unlike the simulator's dev-convenience ticker).

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

Running on a device/simulator (`npm run ios` / `npm run android`) requires
the native projects — see [`NATIVE_SETUP.md`](NATIVE_SETUP.md) first. No
device/native-project run has been done in this environment; typecheck,
lint, and the test suites above are what's been verified.
