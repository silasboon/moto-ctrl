# Wiring MOTO-CTRL to a motorcycle

**Read [`DISCLAIMER.md`](../DISCLAIMER.md) in full before wiring anything
to your bike.** MOTO-CTRL switches lighting, signaling, ignition, and
starter circuits. It is not a certified automotive device. You are
responsible for verifying this wiring is correct and safe for your
specific motorcycle, fusing every circuit appropriately, and bench-testing
before you ride.

This guide covers the general procedure and terminal layout that apply to
any installation. It cannot cover your specific motorcycle's wire colors,
connectors, or circuit locations — those vary by make, model, and year.
Use your bike's factory wiring diagram (or a multimeter and patience) to
identify the actual circuits before connecting anything.

## Before you start

- **Every output channel is generic hardware — function is assigned in the
  app, not by which terminal you use.** OUT1 through OUT12 (CN1) are twelve
  electrically identical high-side switched outputs (see
  [`hardware/PINOUT.md`](../hardware/PINOUT.md)); there is no "the brake
  light terminal" printed on the board. You wire a given motorcycle circuit
  to whichever output terminal is physically convenient, then use the
  app's Pin Mapper screen to tell that channel what it is (`headlight_hi`,
  `headlight_lo`, `brake`, `turn_l`, `turn_r`, `horn`, `ignition`,
  `starter`, or `aux`) and give it a friendly name. Decide your layout on
  paper first, wire it, then configure it in the app to match — and label
  your wiring (tags, a diagram taped in the seat pocket) so a future you
  or a mechanic isn't guessing.
- **Bench-test before installing.** Power the board on the bench, pair it
  with the app, and exercise every output and input from the app before
  you connect it to the bike's actual loads. Confirm outputs switch, LEDs
  (LED1–LED12 mirror OUT1–OUT12 on the board itself) light up, and inputs
  register button presses.
- **Fuse every output for the load actually connected to it.** Match the
  fuse to the wire gauge and the load's actual current draw, not to the
  PROFET's absolute maximum — check the BTS7008-2EPA datasheet and this
  hardware revision's BOM (`hardware/releases/<rev>/bom/`) for the
  per-channel rating before assuming a number.
- **Keep a stock ignition kill switch and any stock immobilizer functional
  as a fallback** unless you fully understand what you're removing (see
  `DISCLAIMER.md`).

## Power

Board power comes in on the 4× M3 ring-terminal pads (main +12V and GND,
dual feed — see `hardware/PINOUT.md`'s "External connectors" section).
Operating range is 10–15V, LiFePO4-compatible. Connect to the bike's main
12V feed and a solid chassis/battery
ground, fused appropriately for the board plus everything it switches.

## Outputs (CN1, 12-pin terminal)

CN1 is a KF142V-5.08-12P terminal block: pin N = OUT*N* for N = 1…12
(`hardware/PINOUT.md`). Each output is a PROFET high-side switch — it
supplies switched +12V to the load; the load's other side returns to
chassis ground, same as any factory-wired 12V lighting/accessory circuit.

Typical functions you'd assign in the Pin Mapper (see
[`docs/PROTOCOL.md`](PROTOCOL.md) §9 for the full field list): headlight
high/low beam, brake light, left/right turn signals, horn, ignition
(switched feed to the ignition system — the interlocked one, see below),
starter (relay coil — see the starter section below, this one is special),
and a few spare `aux` channels for accessories (heated grips, driving
lights, etc.).

Mode per channel (also set in Pin Mapper) controls how a channel switches,
independent of what it's wired to:

- **on/off** — plain digital switching. The right default for anything
  that isn't a turn signal or brake light — including LED lighting, which
  can flicker or misbehave under PWM.
- **PWM dimmed** — steady dimmed brightness while commanded on. Off by
  default per-channel; only turn it on for loads you've confirmed tolerate
  PWM (check with incandescent bulbs or PWM-friendly LED drivers first).
- **turn-signal blink / brake flasher** — full on/off switching at a
  configurable rate, never partial duty, so it works with any lamp type
  including LED turn signals with no separate flasher relay or load
  resistors needed. Brake flasher patterns aren't legal in every
  jurisdiction — check your local vehicle code before enabling one (see
  the in-app note); it's off by default.

### Starter output — hardware button only, wired specially

The starter output is intentionally **not triggerable from the app** — only
the physical handlebar starter button (via CN2, below) can fire it, and
only when the immobilizer isn't `LOCKED` and the firmware's voltage-based
`engine_running` detection says the engine isn't already running. Wire it
the way you'd wire any
starter-relay trigger circuit: the output drives the starter relay coil,
not the starter motor directly. If your bike has (or you're adding) a
neutral/clutch safety interlock, wire that switch to one of the eight
button inputs and assign it as the `starter_interlock_input` in the Pin
Mapper — it's optional but recommended.

### Brake light — priority over any flasher pattern

If you enable a brake-flasher attention pattern, the brake output is
still guaranteed solid-on for the entire time the brake input is asserted
— the flash pattern is only a prefix. Wire the brake switch input (front
and/or rear brake switch, whichever your bike already
has, or both wired to the same input) to one of the eight button inputs
and assign it as `brake_switch_input`.

## Inputs (CN2, 8-pin terminal, active-low)

CN2 is a KF142V-5.08-8P terminal block, wired in **reverse order** from
what you might expect — see `hardware/PINOUT.md`:

| CN2 pin | Signal |
|---|---|
| 1 | BTN8 |
| 2 | BTN7 |
| 3 | BTN6 |
| 4 | BTN5 |
| 5 | BTN4 |
| 6 | BTN3 |
| 7 | BTN2 |
| 8 | BTN1 |

Each input is active-low (pulled up internally; a button/switch should
short the input to ground when pressed/closed — no external pull-up
needed). Assign each input's short-press/long-press/double-press action in
the app, and use the dedicated fields for the two inputs that have a fixed
special meaning: `starter_interlock_input` and `brake_switch_input`
(Pin Mapper, both optional). The button cheat-code (immobilizer fallback
unlock, see `docs/PROTOCOL.md` §11) is entered as a sequence of these same
handlebar button presses — decide your cheat-code layout with your final
button wiring in mind.

## Phone-as-key and the immobilizer

Nothing extra to wire for phone-as-key — it's BLE, not a physical
connection. If you enable the immobilizer, the button cheat-code (via
whichever inputs you wired to CN2) is always available as a fallback even
with phone-as-key enabled — you cannot wire yourself out of the bike.
Optional traditional ignition-switch mode uses one of the
eight inputs the same way as the starter interlock/brake switch above.

## After wiring: verify before riding

Work through [`docs/HARDWARE_TESTING.md`](HARDWARE_TESTING.md)'s bench
checklist — with the board still on the bench if possible, then again
once installed but before starting the engine, then a short stationary
test with the engine running (starter interlock, charging-voltage
detection) before an actual ride.
