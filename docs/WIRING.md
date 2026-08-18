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
  app's Outputs screen to give that channel a name, pick how it switches,
  and tick the few role flags that carry real behaviour. Decide your layout
  on paper first, wire it, then configure it in the app to match — and label
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

There is no fixed list of functions to pick from: a channel gets a
free-text name you choose ("Low Beam", "Heated Grips"), so name it after
whatever you actually wired to it.

**Behaviour** (Outputs screen) controls how a channel switches, independent
of what it's wired to:

- **toggle** — latching on/off. The right default for anything that isn't a
  turn signal or brake light.
- **momentary** — on only while its trigger is held (a held button, or a
  maintained switch like a brake lever).
- **blink** — full on/off flashing at a configurable rate. Turn signals,
  hazards, and anything you want blinking alongside them.
- **flasher** — a short attention-pulse burst on switch-on, then solid.
  Intended for the brake light.

Blink and flasher are always full on/off switching, never partial duty, so
they work with any lamp type including LED turn signals — no separate
flasher relay or load resistors needed. Brake flash patterns aren't legal
in every jurisdiction; check your local vehicle code before enabling one
(see the in-app note), and note it's off by default.

**PWM dimming** is separate from behaviour: set a channel's duty below 100%
and it dims whenever it's driven on. Off by default per channel — only turn
it on for loads you've confirmed tolerate PWM (LED lighting in particular
can flicker or misbehave under it). It never applies to blink or flasher.

**Role flags** are the only channel properties that carry safety logic, and
each is ticked explicitly rather than inferred from a name (see
[`docs/PROTOCOL.md`](PROTOCOL.md) §9 for the full field list):

- `essential` — never shed by the low-voltage cutoff. Tick it on anything
  that must not go dark or dead mid-ride: headlight, ignition, brake light,
  fuel pump.
- `is_ignition` — the immobilizer's target, and what the firmware reads to
  decide the bike is "running". At most one channel.
- `is_starter` — the starter output, which is special (see below). At most
  one channel.
- `is_brake` — the brake light, driven directly from the brake switch input.
- `indicator` (left/right) — a turn signal, which gets mutual exclusion and
  the auto-cancel timer. A channel that merely blinks along with the hazards
  (a DRL, say) is not an indicator — mark it a hazard member instead.

### Starter output — hardware button only, wired specially

The starter output is intentionally **not triggerable from the app** — only
the physical handlebar starter button (via CN2, below) can fire it, and
only when the immobilizer isn't `LOCKED` and the firmware doesn't believe
the engine is already running. Two independent checks decide that: if you've
assigned an output channel as the ignition (see below), that channel being
off always blocks the starter, with no configuration needed — an engine
can't be running with its ignition off. Voltage-based detection (reading
the charging line) is a second, **opt-in** layer you can turn on in
Diagnostics if you want the same protection to react automatically once the
engine is actually turning over; it's off by default because a booster pack
or jump box on the battery terminals looks identical to a running
alternator, and you don't want the starter refused on exactly the bike
you're trying to jump-start. Wire the starter output the way you'd wire any
starter-relay trigger circuit: it drives the starter relay coil, not the
starter motor directly. If your bike has (or you're adding) a neutral/clutch
safety interlock, wire that switch to one of the eight button inputs and
assign it as the `starter_interlock_input` on the Outputs screen — it's
optional but recommended.

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
(Outputs screen, both optional). The button cheat-code (immobilizer fallback
unlock, see `docs/PROTOCOL.md` §11) is entered as a sequence of these same
handlebar button presses — decide your cheat-code layout with your final
button wiring in mind.

## Phone-as-key and the immobilizer

Nothing extra to wire for phone-as-key — it's BLE, not a physical
connection. The immobilizer cannot be enabled until you've configured at
least one non-phone way in, so you cannot wire yourself out of the bike:
either the button cheat-code (entered on whichever inputs you wired to CN2)
or a traditional ignition-switch input. That switch uses one of the eight
inputs the same way as the starter interlock/brake switch above. Whichever
you configure stays available even with phone-as-key enabled, and you can
set up both.

## After wiring: verify before riding

Work through [`docs/HARDWARE_TESTING.md`](HARDWARE_TESTING.md)'s bench
checklist — with the board still on the bench if possible, then again
once installed but before starting the engine, then a short stationary
test with the engine running (starter interlock, charging-voltage
detection) before an actual ride.
