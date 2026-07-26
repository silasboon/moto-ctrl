# Contributing to MOTO-CTRL

Thanks for your interest in MOTO-CTRL. Please read this file before
contributing.

## Ground rules

1. **The safety requirements below are not up for debate in a PR.** If
   your change would weaken ride-safe failure behavior, the immobilizer
   state machine, layered unlock, brake-light priority, starter protection,
   or battery protection, it will not be merged regardless of what else it
   improves. Raise the tradeoff as an issue first.
2. **No cloud, no telemetry, no accounts.** Ever. Not even opt-in. See
   "No cloud, no telemetry, no accounts" below.
3. **No copyleft code.** Don't copy-paste from GPL/LGPL/AGPL-licensed
   sources. If you're adding a new dependency, check its license first.
4. **Pin definitions come from [`hardware/PINOUT.md`](hardware/PINOUT.md)
   only**, via the single generated board-config header. Don't hardcode a
   GPIO number anywhere else.
5. **This project is noncommercial-licensed** (see
   [`LICENSE-NOTE.md`](LICENSE-NOTE.md)). By contributing, you agree your
   contribution is licensed under the same terms as the file(s) you're
   changing ([`LICENSE-CODE`](LICENSE-CODE) for `firmware/`, `app/`,
   `tools/`; [`LICENSE-HARDWARE`](LICENSE-HARDWARE) for `hardware/`,
   `enclosure/`, `docs/`).

## Non-negotiable safety requirements

These override all feature requests, refactors, and stylistic preferences.
If a change would violate one of these, raise it as an issue instead of
implementing it:

1. **Ride-safe failure.** A BLE disconnect, app crash, or phone absence
   must never change output state. If the MCU reboots (watchdog or
   brownout), outputs must be restored from persisted state in under
   250ms. Headlight, ignition, and brake-light channels may never be
   dropped by any software error path while the bike is running.
2. **Immobilizer only engages when stopped/off.** Lock state may only be
   entered from the "parked" state — never while the ignition output is
   live.
3. **Layered unlock — never lock the rider out.** All configured unlock
   methods (phone-as-key, button cheat-code, optional ignition-switch
   input) work simultaneously. The button cheat-code is always available
   as a fallback even when phone-as-key is enabled. A physical factory
   reset (hold BOOT during power-on for 10s, once the board is already
   running) wipes bonds and config after a distinct confirmation.
4. **Phone-as-key must be cryptographically sound.** BLE bonding with LE
   Secure Connections plus an application-layer challenge-response — a
   bare MAC address is never trusted. Multiple paired phones, key
   revocation, and an ownership-transfer flow (full wipe + re-pair) are
   all supported.
5. **Brake light priority.** Any brake-flasher attention pattern must
   guarantee the brake light is on whenever the brake input is asserted —
   the flash pattern is only a prefix, configurable, and off by default
   (flash patterns aren't legal in every jurisdiction).
6. **Starter protection.** The starter output is not triggerable from the
   app — hardware button only — is inhibited while the engine is already
   running (voltage-based detection), and supports an optional
   neutral/clutch interlock input.
7. **Battery protection.** A low-voltage cutoff (configurable, default
   11.8V for LiFePO4) disables non-essential outputs and eventually sleeps
   the board, but never disables the unlock path.

## No cloud, no telemetry, no accounts

MOTO-CTRL is offline-first with zero cloud dependency, by design,
permanently: no backend API, no phone-home telemetry, no analytics SDKs,
no crash reporters that phone out, no user accounts, no login, no
server-side state of any kind. The app talks to the board directly over
BLE and to nothing else over the network, with exactly one narrow,
explicit exception: checking for and downloading firmware updates, which
is two anonymous, unauthenticated HTTPS GETs to a single fixed URL that
never blocks or degrades BLE control if it fails (see
[`docs/FAQ.md`](docs/FAQ.md) for the details). Don't add anything else
that phones home, even if it would make a feature easier — that's a sign
to redesign the feature to work offline instead.

## Development setup

- **Firmware:** see [`firmware/README.md`](firmware/README.md) — requires
  the ESP-IDF toolchain, or just CMake for the host simulator
  (`firmware/sim/`).
- **App:** see [`app/README.md`](app/README.md) — `npm install` in `app/`,
  then `npm run typecheck`, `npm run lint`, `npm test`.

## Making a pull request

- Keep PRs scoped to one feature area where possible. Large PRs spanning
  many unrelated areas are harder to review and more likely to hide a
  safety regression.
- Include tests for anything touching a state machine (lock, output
  engine, combo/cheat-code detection) — these need the most coverage.
- CI (GitHub Actions) builds firmware, builds and tests the host simulator,
  and typechecks/lints/tests the app on every PR. All of it must pass.
- Describe what you tested and how, especially for anything touching
  outputs, the immobilizer, or BLE auth — "it compiles" is not enough for
  safety-relevant changes.

## Reporting bugs / requesting features

Use the issue templates. If you've found a safety-relevant bug (something
that could leave outputs in a wrong state, bypass the immobilizer's
stopped/off requirement, or weaken the unlock/auth flow), say so explicitly
in the title — those get priority.

## Hardware contributions

Hardware source files live under `hardware/releases/<rev>/`, licensed under
CC BY-NC-SA 4.0. If you're proposing a hardware revision, open an issue
first to discuss it before doing the design work — hardware changes affect
`hardware/PINOUT.md`, which is a contract the firmware depends on.

If you're bringing up or testing physical hardware, see
[`docs/FLASHING.md`](docs/FLASHING.md) for programming it,
[`docs/WIRING.md`](docs/WIRING.md) for installing it on a bike, and
[`docs/HARDWARE_TESTING.md`](docs/HARDWARE_TESTING.md) for the bench
checklist to work through — please report back (in an issue) anything that
checklist catches, since none of it has been run against real hardware
yet.
