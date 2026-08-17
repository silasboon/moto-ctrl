# Hardware bench validation checklist

**Status: unexecuted.** No MOTO-CTRL board has been built yet (every other
doc in this repo says the same thing) — this checklist exists so that
whoever brings up the first board, and every release after that, has a
concrete procedure instead of re-deriving one. Check items off as you go;
keep a copy per revision/release (see the sign-off section at the bottom).

## Relationship to the software test pyramid

[`docs/TESTING.md`](TESTING.md) covers what's already proven without any
board: the portable core against synthetic time (`ctest`), the full wire
protocol against a real spawned simulator (the Node itest suite), and
QEMU boot validation of the real cross-compiled binary. Read that doc's
own caveats before assuming this checklist is redundant with any of it:

- QEMU **cannot** validate BLE (no radio controller emulation) — pairing,
  bonding, auth, and anything over the air is bench-only, first proven
  here.
- The simulator's `engine_running`/battery/current values are injected
  directly over a debug channel — real ADC readings, real PROFET current
  sense, and real voltage dividers have never been exercised by any
  automated test. Calibration (`DIAG_SET_CALIB`) is meaningless until it's
  been done against this board's actual analog front end.
- Timing (watchdog restore budget, debounce, BLE reconnect) has only ever
  been measured against synthetic/simulated time or a desktop OS's
  wall clock — never the real MCU's timers under real load.

This checklist is where all of that gets proven for the first time. Don't
read a clean bench run as re-validating logic already covered by
`docs/TESTING.md` — it isn't; it's validating that the logic still holds
once real silicon, real analog signals, and real radio are involved.

## Before you start

- Flash the board per [`docs/FLASHING.md`](FLASHING.md) and confirm boot
  log output over `idf.py monitor` before doing anything else below.
- Nothing in this checklist should be run with the board wired into an
  actual motorcycle — bench power and bench loads only, until the very
  last section.
- Have a way to inject a variable 10–15V supply (a bench PSU) for the
  battery/voltage-threshold tests, and spare bulbs/LEDs/relays to use as
  test loads on the outputs (see [`docs/WIRING.md`](WIRING.md)).

## 1. Power-up

- [ ] Board powers up from bench 12V within the documented 10–15V range
      with no visible damage, smoke, or excessive heat from any component.
- [ ] Quiescent/idle current at rest is in a sane range; re-measure
      specifically in parked/advertising mode against the <2mA average
      target (`CONTRIBUTING.md` safety requirement #7) — this is a real
      current-clamp measurement, not something any simulator run can tell
      you.
- [ ] 3V3 rail present and stable (LED13 power indicator, per
      `hardware/PINOUT.md`).

## 2. Outputs (bench, one channel at a time)

For each of OUT1–OUT12:

- [ ] Commanding the channel on from the app switches a test load (bulb or
      LED + resistor) connected to that terminal, and the corresponding
      board LED (LED1–LED12) lights up.
- [ ] Commanding it off turns the load off, LED included.
- [ ] No other channel is affected (rules out a CN1 wiring/PROFET
      addressing mistake).
- [ ] PWM mode (if you enable it for a test channel): load visibly dims at
      partial duty and follows `pwm_duty_pct` changes from the app.
- [ ] Turn-signal / brake-flasher modes: real blink/pulse timing looks
      right by eye and roughly matches the configured period — the exact
      cycle-accurate timing is already unit-tested against synthetic time
      (`test_output.c`); this step is a sanity check that real GPIO/LEDC
      timing isn't wildly off, not a re-proof of the pattern logic.

## 3. Current sense / diagnostics

- [ ] `DIAG_GET` reports a plausible nonzero current for an actually-loaded
      channel, and ~0 for an unloaded one.
- [ ] `DIAG_SET_CALIB` round-trips and visibly changes subsequent reported
      current (confirms the wire op reaches real calibration storage, not
      just a no-op).
- [ ] Perform an actual gain/offset calibration against a known load and a
      multimeter on this board's IS line, and record the resulting
      `isGain`/`isOffsetMv`/`kilis`/`vbatGain`/`vbatOffsetMv` values for
      this specific board (`hardware/PINOUT.md`'s battery divider and
      current-sense notes both call out "calibrate per board" — this is
      that step, and it has never been done against real hardware before
      now).
- [ ] Disconnect a loaded channel's bulb (open-load) and confirm a fault
      is reported within a reasonable time; re-connect and confirm it
      clears.
- [ ] `DIAG_LEARN` against a real energized channel sets a sane open-load
      threshold from the real measured current.

## 4. Battery voltage / low-voltage cutoff

Using a bench PSU in place of the battery:

- [ ] Reported `batteryMv` (STATUS) tracks the PSU voltage reasonably
      accurately after calibration (§3).
- [ ] Sweeping voltage down through the configured cutoff (default
      11.8V for LiFePO4, `CONTRIBUTING.md` safety requirement #7) engages
      the cutoff — non-essential outputs are suppressed, `lvCutoffActive`
      reports true.
- [ ] Sweeping back up past cutoff + hysteresis clears it.
- [ ] Confirm essential outputs (however you've assigned `ignition`, per
      your Pin Mapper config) are **never** suppressed by the cutoff,
      even at the lowest voltage tested.
- [ ] Raising voltage into the configured `engine_run_mv` threshold (charging
      voltage) makes `engine_running` become true (visible via the
      diagnostics config / starter-inhibited behavior in §6).

## 5. Inputs

For each of BTN1–BTN8 (mind CN2's reversed pin order, `hardware/PINOUT.md`):

- [ ] A press registers (short-press action fires, or is visible via the
      GUI/app's live button state if you're driving straight from a
      handlebar switch bench mockup).
- [ ] Long-press and double-press are distinguishable at real human
      button-press speed (debounce/timing constants are unit-tested
      against synthetic time already — this checks real switch bounce on
      your actual switches doesn't break it).
- [ ] Wrong wiring/pin-order mistakes are ruled out: pressing what you
      think is BTN1 actually registers as BTN1, not BTN8.

## 6. Starter output + interlock

- [ ] Confirm the starter output **cannot** be switched on from the app at
      all (no wire op reaches it — `CONTRIBUTING.md` safety requirement #6)
      — this should already be structurally true, but confirm nothing on
      this board's assembly exposes it some other way (e.g. a debug jumper
      left populated).
- [ ] The physical starter button (wired to one of BTN1–BTN8) does fire the
      starter output when the engine isn't running and (if configured) the
      neutral/clutch interlock input is satisfied.
- [ ] With `engine_running` true (§4's voltage-based detection, or the
      actual engine running once installed), confirm the starter button no
      longer fires the output.
- [ ] With a neutral/clutch interlock input configured and open/unsatisfied,
      confirm the starter button is inhibited.

## 7. BLE pairing, bonding, auth

Real radio, first time — QEMU cannot validate any of this:

- [ ] Board advertises as `MOTO-CTRL` and is discoverable from the app.
- [ ] First-time pairing (trust-on-first-use enrollment) succeeds and the
      session authenticates.
- [ ] Reconnecting after a normal disconnect re-authenticates without
      re-enrolling.
- [ ] Reasonable range/RSSI behavior — confirm the connection survives at
      a realistic on-bike phone-to-board distance, and note where it starts
      to degrade.
- [ ] Enroll a second phone, confirm both can authenticate independently,
      revoke one, confirm the revoked one can no longer authenticate.
- [ ] An unauthenticated write (if you can script one, or via the sim GUI's
      equivalent unauth test pattern) is rejected.

## 8. Immobilizer

With the immobilizer enabled and a cheat-code set:

- [ ] Phone-as-key auto-unlock on a fresh authentication actually releases
      a `LOCKED` bike.
- [ ] The button cheat-code unlocks it with no phone involved.
- [ ] Wrong-code lockout/backoff behaves as expected at real human button
      speed, and phone/ignition-switch unlock still work throughout the
      backoff (only the cheat-code itself is gated).
- [ ] Locking is refused while `engine_running` is true or the ignition
      output is live, on real hardware (not just the sim's injected
      state).
- [ ] Ownership transfer wipes keys and disables the immobilizer as
      expected.

## 9. Physical factory reset

> The "flag if this doesn't trigger at all" item that used to sit here was
> right, and it was confirmed by inspection rather than on the bench: the
> old `factory_reset_check()` sampled GPIO0 once, a few hundred ms into
> `app_main()`, and holding BOOT through reset enters UART download mode so
> the firmware never runs. There was no reachable gesture. It is now a
> 5-second arming window watched on the app tick — see
> `firmware/main/factory_reset.h`. **This has not been exercised on real
> hardware yet**; the items below are the first real test of it.

- [ ] Power the board up normally, **without** BOOT held. Within 5 seconds,
      press and hold BOOT for the full 10 seconds. Confirm the distinct
      all-outputs-blink pattern, that the board reboots itself, and that
      bonds and config were actually wiped (re-pairing required, config
      back to defaults).
- [ ] Releasing BOOT before 10 seconds cancels it with no effect. Pressing
      again while the 5-second window is still open still works.
- [ ] Waiting out the 5-second window and *then* holding BOOT for 10s does
      **nothing** — this is what stops a stuck button wiping a bike's
      config mid-ride. Confirm it stays inert for the rest of the run.
- [ ] Board calibration survives the reset (it describes the board, not the
      owner — see `nvs_calib_hal.h`).
- [ ] The confirmation blink does not trip the task watchdog: the pattern
      takes ~1.4s on the watchdog-monitored app task and feeds it between
      cycles. A reboot into a panic here is a bug, not a pass.

## 10. Watchdog / reboot / restore timing

- [ ] Force a reboot (power-cycle or watchdog trip) while outputs are on
      and confirm they're restored from persisted state. Measure the
      actual time to restore and confirm it's under the 250ms budget
      (`CONTRIBUTING.md` safety requirement #1) — on real hardware, not
      simulated ticks.
- [ ] Confirm the two strapping-pin outputs (GPIO3/GPIO46 → PROFET
      IN6/DEN3, `hardware/PINOUT.md`) come up correctly and don't glitch
      the corresponding channel during boot — this is the one pin-handling
      detail `hardware/PINOUT.md` explicitly calls out as needing real
      hardware to confirm, not just review.
- [ ] Corrupt/blank NVS on a **spare** or freshly-erased board and confirm
      safe fallback to defaults rather than a boot loop or crash (the sim
      proves the logic; this confirms real NVS behaves the same way).

## 11. OTA (real hardware, both directions)

- [ ] Flash an initial build over UART (`docs/FLASHING.md`), then perform a
      full OTA update over BLE from the app to a second signed build, and
      confirm the board reboots into the new image (check the reported
      firmware version in STATUS before/after).
- [ ] **Check whether the new image survives a second reboot.**
      `firmware/sdkconfig.defaults` enables
      `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`, but nothing in
      `firmware/main/` currently calls
      `esp_ota_mark_app_valid_cancel_rollback()` (or equivalent) to confirm
      the new image to the bootloader. If that's still true when you run
      this test, expect the bootloader to roll back to the previous image
      on the *second* reboot after an OTA update, even though the update
      itself reported success — that would be a real firmware gap to fix,
      not a wiring or bench-procedure problem. Confirm which behavior you
      actually see and record it here.
- [ ] Attempt an OTA update while `engine_running` is true (or battery
      below the low-voltage cutoff) and confirm it's refused
      (`docs/PROTOCOL.md` §10.3) rather than silently proceeding.
- [ ] Confirm the currently-running image keeps the bike fully operable
      (outputs, immobilizer) throughout an entire OTA transfer, right up
      until the explicit reboot step.

## 12. Full install (last, on an actual motorcycle)

Only after everything above passes on the bench:

- [ ] Wire per [`docs/WIRING.md`](WIRING.md), fused per your bike's actual
      loads.
- [ ] With the engine off, re-verify every output/input/immobilizer
      behavior from §§2, 5, 8 now that they're on real vehicle circuits,
      not bench loads.
- [ ] Start the engine and confirm charging voltage is correctly detected
      as `engine_running` (§4/§6) and the starter is inhibited while
      running.
- [ ] A short stationary test (engine running, stand or center stand) for
      lighting/signal/brake-light behavior before an actual ride.
- [ ] Re-run the safety-critical subset (lights, brake light, starter
      interlock — `DISCLAIMER.md`) after every subsequent firmware update,
      on the bench, before riding again.

## Sign-off

| Field | Value |
|---|---|
| Hardware revision | (`hardware/PINOUT.md`'s revision, e.g. v1) |
| Firmware version | |
| Tester | |
| Date | |
| Result | pass / fail / partial (list open items) |
| Notes | |
