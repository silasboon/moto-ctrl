# FAQ

## What is MOTO-CTRL?

An open-source, programmable motorcycle control board — a 12-output,
8-input ESP32-S3 board that switches your bike's electrical loads
(lights, signals, ignition, starter relay, horn, accessories) over BLE,
with a phone-as-key immobilizer and a companion mobile app. It's meant as
an alternative to closed commercial products like the Motogadget m.Unit
Blue, but open: the design, firmware, and app are all published, and
assembled boards are sold Speeduino-style (open design, commercial boards
funded through the project itself).

## Where's the project up to right now?

See the **Status** line at the top of the repo's [`README.md`](../README.md).
This FAQ doesn't track that day-to-day.

## Is it safe? Is it certified?

No certification, and please read [`DISCLAIMER.md`](../DISCLAIMER.md) in
full before building, flashing, wiring, or riding with one. MOTO-CTRL has
not been through ISO 26262 functional-safety certification, DOT/ECE/UNECE
type approval, or CE/FCC/UKCA compliance testing. It's a hobbyist project.
The firmware follows a specific, non-negotiable set of safety requirements
(see `CONTRIBUTING.md` — ride-safe failure on disconnect/crash, immobilizer
state rules, layered unlock that can't lock you out, brake-light priority,
hardware-only starter, battery protection) that every change is held to,
but "we designed carefully" is not the same thing as "an independent body
certified this." You assume the risk — see the disclaimer for exactly what
that means.

## Can I buy an assembled board?

Only from the MOTO-CTRL project itself — see
[`LICENSE-NOTE.md`](../LICENSE-NOTE.md). Nobody else is authorized to sell
assembled boards, kits, or PCBs made from this design. Whether boards are
actually available yet depends on where hardware production is at — check
the README status line and `hardware/releases/` for what's been built.

## Can I build my own from the design files?

Yes — personal, noncommercial builds are explicitly permitted under both
licenses (code: PolyForm Noncommercial; hardware/docs: CC BY-NC-SA 4.0).
See [`LICENSE-NOTE.md`](../LICENSE-NOTE.md), [`docs/WIRING.md`](WIRING.md),
and [`docs/FLASHING.md`](FLASHING.md).

## Can I sell boards, kits, or a modified version?

No. Selling assembled boards, bare PCBs, kits, or enclosures made from
this design isn't permitted under either license, and modifying the
design doesn't change that — a fork is still noncommercial-restricted.
See [`LICENSE-NOTE.md`](../LICENSE-NOTE.md) for the full breakdown,
including how to ask about commercial licensing arrangements outside the
default terms.

## What phone / OS does the app need?

The app is bare React Native (not Expo), using `react-native-ble-plx` for
BLE. iOS uses `bluetooth-central` background mode with CoreBluetooth state
restoration so phone-as-key can reconnect from a locked phone; Android uses
a foreground-service-based reconnect that respects doze mode. No NFC/UWB
"digital car key" style integration is planned — phone-as-key here is
BLE-proximity based.

## Does the app need an internet connection or an account?

No accounts, ever — full stop. The app talks to the board over BLE and to
nothing else, with exactly one narrow exception: checking for and
downloading firmware updates, which is two anonymous, unauthenticated
HTTPS GETs (a small version-manifest file, then the signed update itself)
to a single fixed URL — no login, no device ID, no analytics, and it never
blocks or degrades BLE pairing/control if it fails (see `CONTRIBUTING.md`'s
"No cloud, no telemetry, no accounts" section for the exact constraints).
BLE pairing, control, config, diagnostics, and the immobilizer all work
with the phone fully offline.

## What happens if my phone dies, I forget it, or BLE just isn't working?

You always have a non-phone way in. Before the immobilizer can be enabled
at all, you must configure at least one method that isn't the phone —
either the handlebar button cheat-code or a traditional ignition-switch
input wired to one of the inputs. Whichever you set stays available the
whole time; it is never disabled just because phone-as-key is also turned
on (layered unlock: you can't lock yourself out). Configure both if you
want two non-phone fallbacks.

## What if my phone is lost or stolen?

Revoke that phone's key from the app while authenticated with another
paired phone (or the cheat-code + a factory reset if you have no other
paired phone) — the device only ever stores public keys, so revoking one
is immediate and doesn't require the missing phone to be present. See
`docs/PROTOCOL.md` §6 for key management, and the ownership-transfer flow
if you're getting rid of the whole bike.

## What happens if the BLE connection drops or the app crashes?

Nothing changes on the board. Output state is never touched by a
disconnect, an app crash, or the phone simply not being there — that's the
project's first, hardest safety requirement. If the MCU itself reboots
(watchdog trip or brownout), outputs are restored from persisted state in
under 250ms.

## How do I fully factory-reset a board?

Hold the BOOT button for 10 seconds **while the board is already powered
on and running normally** (not during power-up — see
[`docs/FLASHING.md`](FLASHING.md)'s callout on why those two are
different) until you see the distinct all-outputs-blink confirmation
pattern. This wipes paired keys and configuration. Do this before
transferring the bike to a new owner, or if you're locked out with no
other recourse.

## How do firmware updates work?

Once a board is flashed and paired, routine updates go over BLE: the app
checks for a new signed release, downloads it, transfers it to the board,
and you choose when to apply it (the current firmware keeps running
throughout the transfer). See `docs/PROTOCOL.md` §10 for the wire protocol
and `tools/sign-firmware.py` for how releases get signed. The very first
flash of a bare board, or recovery if BLE itself isn't working, uses UART
instead — see [`docs/FLASHING.md`](FLASHING.md).

## Can I use a flashing turn-signal / brake-light attention pattern?

The board supports it (full on/off switching, so it works with any lamp
type including LED turn signals — no hyperflash workarounds needed), but
it's **off by default**, and flash patterns on brake lights aren't legal
in every jurisdiction — check your local vehicle code before enabling one.

## Why no USB? Why do I need a separate UART adapter to flash it?

Hardware simplicity — the board has a 3-pin UART header instead of a USB
port or auto-reset circuitry. See [`docs/FLASHING.md`](FLASHING.md) for
the manual BOOT/EN sequence this requires.

## Why no WiFi?

The ESP32-S3 module has WiFi hardware, but v1 firmware doesn't use it —
BLE 5 only. This keeps the board's radio surface and power budget smaller,
and there's no feature in this project that needs WiFi (the one network
exception — update checking — runs on the phone, not the board).

## Why can't the app trigger the starter directly?

Because that's exactly the kind of thing that shouldn't be remotely
triggerable — the starter output is only reachable from the physical
handlebar button, is inhibited whenever the firmware detects the engine
is already running (via charging voltage), and supports an optional
neutral/clutch interlock.

## I found a bug. Where do I report it?

Use the issue templates (see [`CONTRIBUTING.md`](../CONTRIBUTING.md)). If
it's safety-relevant — outputs could end up in a wrong state, the
immobilizer's stopped/off requirement could be bypassed, or the unlock/auth
flow could be weakened — say so explicitly in the title; those get
priority.

## Can I contribute?

Yes — see [`CONTRIBUTING.md`](../CONTRIBUTING.md) for the ground rules
(the safety requirements there aren't up for debate in a PR), development
setup, and what a good PR looks like.
