# MOTO-CTRL

An open-source programmable motorcycle control board — an alternative to the
Motogadget m.Unit Blue. 12 switched outputs, 8 handlebar button inputs, BLE
phone-as-key immobilizer, and a companion mobile app, built around an
ESP32-S3. Open design, sold assembled by the project — Speeduino-style.

**MOTO-CTRL is feature-complete for its first release.** The firmware core,
BLE protocol, a WebSocket simulator with a browser debug GUI and QEMU boot
validation, the companion app (pairing, dashboard, pin mapper, output
control), the phone-as-key immobilizer, diagnostics (current sensing,
learnable fault thresholds, low-voltage cutoff), turn signals/hazard/brake
flasher with PWM dimming, and OTA firmware updates (signed images over BLE
with A/B partitions, a persisted security/safety event log, and a
forward-tolerant config format) are all implemented and covered by the
app's Firmware Update and Event Log screens and the maintainer
release-signing tool (`tools/sign-firmware.py`). Verified against the
simulator and host unit tests — no physical board has been built yet. See
[`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the wire protocol,
[`docs/TESTING.md`](docs/TESTING.md) for how all of this is tested, and
[`docs/WIRING.md`](docs/WIRING.md) / [`docs/FLASHING.md`](docs/FLASHING.md) /
[`docs/HARDWARE_TESTING.md`](docs/HARDWARE_TESTING.md) / [`docs/FAQ.md`](docs/FAQ.md)
for installing, flashing, and bench-testing an actual board once one
exists.

## Before anything else

- [`DISCLAIMER.md`](DISCLAIMER.md) — this is not a certified automotive
  safety device. Read it before wiring anything to a motorcycle.
- [`LICENSE-NOTE.md`](LICENSE-NOTE.md) — plain-English licensing summary
  (personal use/builds OK, selling boards or derivatives is not).
- Contributing? See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the ground
  rules before opening a PR.

## Repository layout

```
moto-ctrl/
├── firmware/          ESP-IDF (C, FreeRTOS, NimBLE) firmware + host simulator
├── app/                bare React Native + TypeScript companion app
├── hardware/           schematic, PCB design, gerbers, BOM (per hardware revision)
│   ├── PINOUT.md        firmware ↔ hardware pin contract — read before touching pins
│   └── releases/         e.g. releases/v2/, matching git tags
├── enclosure/          STL/STEP files (FDM ASA prototype, MJF PA12 production)
├── docs/               wiring guide, flashing guide, protocol spec, FAQ, hardware bench checklist
├── tools/              OTA release signing (sign-firmware.py), native app bootstrap
└── .github/workflows/  CI: firmware build, app build, tests
```

## Hardware

MCU: ESP32-S3-WROOM-1-N4 (4MB flash, no PSRAM), BLE 5. 12 outputs via 6×
Infineon BTS7008-2EPA high-side PROFETs with current sense and diagnostics.
8 active-low handlebar button inputs. 12V motorcycle power, LiFePO4-compatible.
UART0-only programming (no USB). See [`hardware/PINOUT.md`](hardware/PINOUT.md)
for the full pin contract.

## Software stack

- **Firmware:** ESP-IDF + NimBLE, C, FreeRTOS. A host simulator
  (`firmware/sim/`) builds the core state machine and protocol on
  Linux/macOS against a fake BLE transport, so the app and CI don't need
  real hardware.
- **App:** bare React Native + TypeScript (no Expo), BLE via
  `react-native-ble-plx`. Offline-first, zero cloud dependency, no accounts.

## Safety-critical behavior

MOTO-CTRL switches lighting, signaling, ignition, and starter circuits on a
moving vehicle. A handful of safety rules override every feature request
and are not up for debate in a PR: a BLE disconnect, app crash, or phone
absence never changes output state; the immobilizer can only lock while
the bike is stopped and off; the button cheat-code fallback always works
even with phone-as-key enabled, so you can't lock yourself out; phone-as-key
uses bonded, cryptographically signed challenge-response, never a bare MAC
address; the brake light is always on whenever the brake is pulled,
attention-flash pattern or not; the starter is never remotely triggerable
and is inhibited while the engine is already running; and low-battery
protection never disables the unlock path. See
[`CONTRIBUTING.md`](CONTRIBUTING.md) before proposing a change to lock,
output, or auth behavior, and [`DISCLAIMER.md`](DISCLAIMER.md) for what
this does and doesn't mean for your own liability.

## License

- Code (`firmware/`, `app/`, `tools/`): [PolyForm Noncommercial 1.0.0](LICENSE-CODE)
- Hardware & docs (`hardware/`, `enclosure/`, `docs/`): [CC BY-NC-SA 4.0](LICENSE-HARDWARE)
- Summary: [`LICENSE-NOTE.md`](LICENSE-NOTE.md)

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md).
