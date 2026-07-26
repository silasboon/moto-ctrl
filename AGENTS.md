# AGENTS.md — MOTO-CTRL agent instructions

This is the canonical instructions file for any AI coding agent (Claude Code,
other AI tools, etc.) working in this repository. It exists so that any agent
picking up this project — in this session or a future one — can work safely from
this file alone, without needing the original project-kickoff conversation.

`CLAUDE.md` at the repo root is just a pointer to this file. If you are an agent
and you only read one file before starting work, read this one.

## What MOTO-CTRL is

MOTO-CTRL is an open-source programmable motorcycle control board (an alternative
to the Motogadget m.Unit Blue): a 12-output, 8-input ESP32-S3 board that switches
motorcycle electrical loads (lights, signals, ignition, starter, horn, aux) over
BLE, with a phone-as-key immobilizer, a companion mobile app, and a hardware
design that's sold assembled by the project — Speeduino-style: open design,
commercial boards.

## Non-negotiable safety requirements

These override all feature requests, refactors, "quick fixes," and stylistic
preferences. If a change would violate one of these, stop and raise it instead of
implementing it. These are recorded verbatim from the project's founding
specification and must not be edited, softened, or reinterpreted away:

1. **Ride-safe failure.** BLE disconnect, app crash, or phone absence must NEVER
   change output state. If the MCU reboots (watchdog or brownout), outputs must be
   restored from persisted state in <250ms. Headlight, ignition, and brake-light
   channels may never be dropped by any software error path while the bike is in
   the "running" state.
2. **Immobilizer only engages when stopped/off.** Lock state may only be entered
   from the "parked" state — never while ignition output is live.
3. **Layered unlock — never lock the rider out.** All configured unlock methods
   work simultaneously: (a) phone-as-key BLE proximity/tap, (b) button cheat-code
   on the handlebar buttons, (c) optional traditional ignition-switch input mode.
   The button code is always available as fallback even when phone-as-key is
   enabled. Physical factory reset: hold BOOT during power-on for 10s → wipes
   bonds + config after a distinct LED pattern confirmation.
4. **Phone-as-key must be cryptographically sound.** BLE bonding with LE Secure
   Connections + application-layer challenge-response (device-stored key signs a
   per-session nonce). MAC address alone is never trusted. Support multiple
   paired phones, key revocation, and an ownership-transfer flow (full wipe +
   re-pair).
5. **Brake light priority.** Any brake-flasher pattern must guarantee the brake
   light is ON whenever the brake input is asserted; flash pattern is an
   attention prefix, configurable, defaulting to a short 3-pulse burst then
   solid. Include a note in the app that flash patterns are not legal in all
   jurisdictions, default OFF.
6. **Starter protection.** Starter output is not triggerable from the app
   (hardware button path only), is inhibited while the engine-running state is
   detected (voltage-based detection: charging voltage > threshold), and
   supports an optional neutral/clutch interlock input assignment.
7. **Battery protection.** Parked mode: deep sleep or low-power BLE advertising,
   target <2mA average. Low-voltage cutoff (configurable, default 11.8V for
   LiFePO4) disables non-essential outputs and eventually sleeps hard. Never
   fully disable the unlock path.

## Pin definitions: single source of truth

- [`hardware/PINOUT.md`](hardware/PINOUT.md) is the firmware ↔ hardware contract.
  It is authoritative and is generated from the schematic for each hardware
  revision (see `hardware/releases/<rev>/`).
- All GPIO assignments in firmware **must** come from one generated board-config
  header (`firmware/components/board_config/`). No `.c`/`.h` file outside that
  component may reference a raw `GPIOxx` number.
- If `PINOUT.md` changes (new hardware revision), regenerate the board-config
  header from it — do not hand-edit pins in both places and let them drift.
- Two pins (GPIO3, GPIO46) are ESP32-S3 strapping pins wired to PROFET IN6/DEN3.
  They are verified safe on this hardware, but firmware must never enable
  internal pullups on them before boot completes, and must drive them low during
  early init. Do not "clean up" this handling without re-reading the warning in
  `PINOUT.md`.

## Licensing model

- **Code** (`firmware/`, `app/`, `tools/`, any source in this repo): PolyForm
  Noncommercial 1.0.0 — see [`LICENSE-CODE`](LICENSE-CODE).
- **Hardware & docs** (`hardware/`, `enclosure/`, `docs/`): CC BY-NC-SA 4.0 — see
  [`LICENSE-HARDWARE`](LICENSE-HARDWARE).
- Plain-English summary: [`LICENSE-NOTE.md`](LICENSE-NOTE.md). Personal use and
  personal builds are fine. Selling boards, kits, PCBs, enclosures, or
  derivatives built from this design is not permitted. Assembled boards are sold
  only by the MOTO-CTRL project itself.
- **Never introduce GPL, LGPL, AGPL, or any other copyleft-licensed code,
  library, or snippet into this repository**, including via copy-paste from a
  copyleft-licensed source. Copyleft terms are incompatible with the
  noncommercial-but-otherwise-permissive model this project uses and would
  create a licensing conflict for downstream users.
- Permissively-licensed dependencies are fine: ESP-IDF (Apache 2.0), NimBLE
  (Apache 2.0), React Native and its standard ecosystem (MIT), and similarly
  MIT/BSD/Apache-licensed libraries. If you are about to add a new dependency,
  check its license first; if it's copyleft or unclear, stop and ask rather than
  adding it.

## No cloud, no telemetry, no accounts — ever

MOTO-CTRL is offline-first with zero cloud dependency, by design, permanently:

- No cloud services, no backend API, no phone-home telemetry, no analytics SDKs,
  no crash reporters that phone out, no user accounts, no login, no server-side
  state of any kind.
- The app talks to the board directly over BLE and to nothing else over the
  network. The firmware simulator talks to the app over a local TCP/websocket
  transport, not a hosted one.
- Do not add any of the above even if it would make a feature easier (e.g. "sync
  config across phones via a cloud store," "send crash reports to a dashboard").
  If a feature seems to need it, that's a sign to redesign the feature to work
  offline, not to add the dependency.
- This rule is not covered by the phase-gate process below — it applies to every
  phase and every future addition, without exception, and does not require
  re-confirmation each time.

### Exception: firmware update check/download

**These two requests are the only outbound network traffic permitted anywhere
in this repository** — app, firmware, tools, and any future component. Any
other socket, HTTP client, DNS lookup, or SDK that opens a connection at
runtime is a violation of the rule above, not a candidate for a second
exception.

The two permitted requests are:

1. **Manifest fetch.** A read-only GET of a small static version-manifest
   file (latest version, changelog, bundle URL, bundle SHA-512, bundle size)
   to show "update available" in the app.
2. **Bundle download.** A read-only GET of the signed `.mcota` firmware
   bundle that manifest names.

**Trigger — user-initiated only.** Both requests may fire only as the direct
result of the rider tapping a control in the app: the manifest fetch from an
explicit "Check for updates" action, the bundle download from an explicit
"Download this update" action after the rider has seen the version and
changelog. Specifically prohibited: fetching on app launch, on screen mount,
on BLE connect, on a timer/interval, from a background task, from a push
mechanism, or as a silent prefetch. No automatic or background polling of any
kind. The rider must be able to use the app indefinitely, in full, without a
single packet leaving the device.

**Destination — one baked-in host.** The manifest URL is a single fixed
constant compiled into the app (`UPDATE_MANIFEST_URL`, currently a
`github.com` Releases asset). It is not configurable at runtime, not
overridable by the device, and not read from BLE, config, or storage. The
bundle URL comes from the manifest and is therefore remote input: it must be
validated before use, and rejected unless it is `https://` **and** on the
same host as `UPDATE_MANIFEST_URL`. A manifest naming any other host is a
malformed manifest and the update must fail closed. Redirects to a different
host are likewise not followed. These are the only two hosts-worth of
traffic — in practice, one host.

**Protocol — HTTPS with certificate validation, mandatory.** Both requests
are HTTPS. TLS certificate and hostname validation must be fully enforced;
plaintext HTTP is never acceptable for either request, not even as a
fallback when TLS fails. Certificate validation must never be disabled,
relaxed, or exempted for these hosts — no `NSAllowsArbitraryLoads` /
per-domain ATS exception on iOS, no `usesCleartextTraffic` or custom
`TrustManager` / `network_security_config` exemption on Android, no
"insecure" or "rejectUnauthorized: false" option in any HTTP client. A TLS
failure is a failed update check, not a reason to retry insecurely.

Further constraints, all mandatory:

- No user data, device identifiers, analytics, or telemetry may be sent in
  either request — these must be anonymous, unauthenticated GETs of static
  files, equivalent to opening the URL in a browser. No custom headers
  carrying identifying information, no query parameters, no cookies, no
  `User-Agent` beyond the platform default.
- No accounts, no request beyond these two GETs, no other host.
- If either request fails or times out, the app must fail silently/show
  "unable to check for updates" — it must never block, delay, or degrade
  BLE pairing/control of the board, which remains fully offline.
- This is the app phoning home, never the board itself — the firmware has no
  WiFi/network stack in v1 (see "Hardware facts firmware code may assume")
  and this exception does not change that.
- The firmware bundle's cryptographic signature (docs/PROTOCOL.md §10) is
  still independently verified on-device by mc_ota — this exception changes
  only how the app obtains the file, not the OTA trust model. The app's own
  size/SHA-512 check against the manifest is transport-integrity only and is
  never the security boundary.
- This exception is scoped strictly to update-check/download. It is not
  precedent for crash reporting, sync, accounts, or any other network
  feature — those remain permanently disallowed.

## Phase-gated workflow

Work proceeds in phases. **After each phase, stop and produce a short summary
for human review before starting the next phase.** Do not chain multiple phases
together in one pass, even if the next phase seems obvious or small.

1. Repo scaffold + CI. *(stop for review)*
2. Firmware core: board config, output engine, input engine, NVS config,
   watchdog + state restore. Unit tests on the sim target. *(stop for review)*
3. BLE stack: pairing/bonding, auth, status/control/config services. Sim
   transport. *(stop for review)*
3.5. Pre-hardware validation harness: a browser debug GUI for the simulator
   (fault injection, virtual buttons, scenario record/replay) and QEMU-based
   boot validation of the real firmware binary in CI. No board exists yet —
   this phase makes Phases 4 and 5 fully developable and testable without
   one. See `docs/TESTING.md`. QEMU validates boot sequence, NVS/config
   restore, and watchdog init on the real target code; it does **not**
   validate BLE (no radio controller emulation) — don't read a passing QEMU
   job as evidence about pairing, bonding, or auth on real hardware.
   *(stop for review)*
4. App MVP: pairing, dashboard, pin mapper, output control. *(stop for review)*
5. Lock system: phone-as-key, cheat-code, ignition-switch mode, immobilizer
   state machine. This phase gets extra test coverage — every state transition.
   *(stop for review)*
6. Diagnostics: IS mux driver, current calibration, faults, blown-bulb
   detection. *(stop for review)*
7. Flashers/PWM: turn signals, auto-cancel timer, brake flasher, dimming.
   *(stop for review)*
8. OTA + config migration + event log. *(stop for review)*
9. Docs: flashing guide (UART, manual boot), wiring guide, protocol spec, FAQ.
   *(stop for review)*

Within a phase, produce working, tested code — not partial/half-finished
implementations. Testing throughout: unit tests for state machines (lock,
output, combo detection are the critical ones), protocol round-trip tests
against the simulator, and a `HARDWARE_TESTING.md` checklist for bench
validation per release.

**Ask before making any architectural decision not covered in this file or in
an existing design doc under `docs/`.** Do not silently resolve ambiguity by
picking whichever interpretation is more convenient to implement.

## Stack choices (fixed — do not swap without asking)

- **Firmware:** ESP-IDF (latest stable) + NimBLE, C, FreeRTOS tasks. Not Arduino.
- **App:** bare React Native + TypeScript (not Expo). BLE via
  `react-native-ble-plx`. All BLE/key logic sits behind a TypeScript transport
  interface with two implementations — the ble-plx transport and a
  simulator TCP/websocket transport — so the app can run against
  `firmware/sim/` in CI with no hardware attached, and so the iOS key path can
  later be swapped for a native Swift module without touching app logic.
- **Host simulator:** `firmware/sim/` builds the core state machine + protocol
  on desktop (Linux/macOS) against a fake BLE transport, for app development
  and CI with no hardware attached.
- iOS: `bluetooth-central` background mode + CoreBluetooth state restoration
  (`restoreStateIdentifier`) so phone-as-key reconnects from a locked phone.
  Android: foreground-service-based reconnect respecting doze mode.
  Phone-as-key is BLE-proximity based — do not add NFC/UWB car-key integrations;
  Apple restricts those APIs to automotive partners.

## Hardware facts firmware code may assume

- MCU: ESP32-S3-WROOM-1-N4 — 4MB flash, **no PSRAM**. Plan memory/partition
  budgets accordingly. BLE 5 only; WiFi is unused in v1 — do not add WiFi-based
  features without asking.
- 12 outputs via 6× Infineon BTS7008-2EPA PROFETs (2 channels/device), each with
  IN0/IN1 control, a shared DSEL diagnostic-channel-select line, per-device DEN,
  and one shared IS current-sense ADC line. PROFETs support PWM, but see the
  PWM/flasher rule below.
- 8 active-low handlebar button inputs (BTN1–BTN8) via CN2.
- Battery sense via a 1MΩ/100kΩ divider (ratio 0.0909) into ADC with 12dB
  attenuation; calibrate in firmware, don't assume ideal resistor values.
- Programming is UART0 only — 3-pin header, no USB, no auto-reset. Manual
  BOOT/EN buttons. Flashing docs must account for manual boot entry.
- GPIO2 is spare (ADC1-capable), reserved for a future NTC board-temp sensor /
  5V rail monitor — leave a driver stub, don't wire real functionality to it
  without a spec.
- Power: 12V motorcycle system, LiFePO4-compatible, 10–15V operating range.
- **PWM/flasher rule:** PWM duty-cycle features (dimming, soft-start) are
  per-channel and OFF by default, because driver-based LED lamps can flicker or
  misbehave under PWM. All flasher patterns (turn, hazard, brake flasher) are
  full on/off switching, never partial duty, so they work with every lamp type
  and LED turn signals never need ballast resistors or hyperflash workarounds.
- Blown-bulb (open-load) current thresholds are per-channel, configurable, and
  learnable — do not hardcode incandescent-bulb current assumptions.

## Documentation this file does not replace

- [`hardware/PINOUT.md`](hardware/PINOUT.md) — pin contract, read before writing
  any pin definition.
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — BLE GATT layout (status/control/
  config/OTA services), written to be implementable by third-party clients.
- [`docs/TESTING.md`](docs/TESTING.md) — the test pyramid, the sim debug GUI
  and its sim-only debug protocol (never confuse with `docs/PROTOCOL.md`),
  scenario record/replay, and what QEMU boot validation does and does not
  prove.
- [`docs/WIRING.md`](docs/WIRING.md) / [`docs/FLASHING.md`](docs/FLASHING.md) /
  [`docs/HARDWARE_TESTING.md`](docs/HARDWARE_TESTING.md) — installing on a
  motorcycle, UART flashing + the manual BOOT/EN bootloader sequence (not
  the same thing as the runtime BOOT-hold factory reset above — see
  `FLASHING.md`'s callout), and the bench validation checklist for when
  real hardware exists.
- [`docs/FAQ.md`](docs/FAQ.md) — user-facing questions already answered;
  check here before re-explaining something like the update-check exception
  or the cheat-code fallback from scratch in an issue/PR response.
- [`LICENSE-NOTE.md`](LICENSE-NOTE.md) / [`DISCLAIMER.md`](DISCLAIMER.md) — read
  before writing anything user-facing about legality, safety, or warranty.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — contributor workflow and PR
  expectations.
