# Hardware bench validation checklist

**Status: worked through on v1 hardware.** This is a per-release procedure,
not a one-time bring-up: run it again for every hardware revision and every
firmware release, since it is the only layer that covers real GPIO, real
current sensing, and real BLE. Check items off as you go; keep a copy per
revision/release (see the sign-off section at the bottom).

## Relationship to the software test pyramid

[`docs/TESTING.md`](TESTING.md) covers what's already proven without any
board: the portable core against synthetic time (`ctest`), the full wire
protocol against a real spawned simulator (the Node itest suite), and
QEMU boot validation of the real cross-compiled binary. Read that doc's
own caveats before assuming this checklist is redundant with any of it:

- QEMU **cannot** validate BLE (no radio controller emulation) — pairing,
  bonding, auth, and anything over the air is bench-only.
- The simulator's `engine_running`/battery/current values are injected
  directly over a debug channel, so no automated test exercises real ADC
  readings, real PROFET current sense, or the real voltage dividers.
  Calibration (`DIAG_SET_CALIB`) is meaningless until it has been done
  against a given board's actual analog front end.
- Timing (watchdog restore budget, debounce, BLE reconnect) is only ever
  measured by the automated layers against synthetic time or a desktop
  OS's wall clock, never the real MCU's timers under real load.

This checklist is where all of that gets proven. Don't read a clean bench
run as re-validating logic already covered by `docs/TESTING.md` — it isn't;
it's validating that the logic still holds once real silicon, real analog
signals, and real radio are involved.

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
      you. See §1.1 for the parked-power procedure.
- [ ] 3V3 rail present and stable (LED13 power indicator, per
      `hardware/PINOUT.md`).

### 1.1 Parked power (mc_power / light sleep)

`mc_power` steps the board down through ACTIVE (10ms tick) → IDLE (100ms,
light sleep) → PARKED (250ms, light sleep, ~1s BLE advertising). Reference
point: before any of this existed the board drew ~18mA continuously, and
~0mA with the ESP32-S3 held in reset — so essentially all of it is
MCU-side and in play.

- [ ] Baseline with EN held low is still ~0mA (confirms no new hardware
      draw crept in).
- [ ] Power up, leave the board completely alone with no phone connected,
      and watch the supply. Current should step down twice: once at
      `MC_POWER_DEFAULT_IDLE_AFTER_MS` (5s) and again at
      `MC_POWER_DEFAULT_PARKED_AFTER_MS` (60s). Record all three levels.
- [ ] **Wake works.** From PARKED, a single handlebar button press must
      wake the board and register as a press — no lost first press. This is
      the highest-risk item in this section: the GPIO interrupt notify and
      the light-sleep wake are separate mechanisms and their interaction is
      only settled on real silicon. If the first press is ever swallowed,
      the 250ms parked tick is the backstop and the notify path needs
      investigating (`input_hal_gpio_wake_init`).
- [ ] **Maintained switch held closed.** Ground an input assigned as the
      ignition switch and leave it grounded for several minutes, then open
      it again. The board must stay up. Regression check: the wake path uses
      a *level*-triggered interrupt (`gpio_wakeup_enable()` rejects edge
      modes and rewrites the pin's interrupt type to match), so a handler
      that does not mask its own pin re-enters forever and panics the
      interrupt watchdog within milliseconds — a maintained switch holds the
      pin low indefinitely, so this is certain rather than rare. Also power
      the board up with the switch *already* grounded, which is the same bug
      arriving during init.
- [ ] **Cheat-code entry from cold.** With the immobilizer engaged and the
      board parked, enter the full cheat-code at normal speed. It must
      unlock — nothing may be dropped mid-sequence. Layered unlock: this is
      the case where a bug locks a rider out of their own bike.
- [ ] **Phone reconnect from parked.** A paired phone must still connect
      and authenticate while the board is in PARKED (slower to be
      discovered is expected — roughly a second — failing to connect is
      not).
- [ ] **Never sleeps in use.** With any output on, the engine running, or
      an OTA in flight, current must stay at the ACTIVE level and blink /
      flasher timing must look unchanged by eye. A visible change in blink
      rate means a hold-awake gate is not doing its job.
- [ ] OTA transfer completes at the same speed as before.
- [ ] If the numbers come in higher than hoped, the first knob to try is
      `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=n` in
      `sdkconfig.defaults` — but re-run the phone-reconnect and
      connection-stability items after changing it, since the main crystal
      is the BLE controller's low-power clock on this board.

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
      that step, and it has to be redone for each individual board).
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
- [ ] Confirm essential outputs (whichever channels you've ticked
      `essential`, plus the `is_ignition`/`is_brake` ones) are **never**
      suppressed by the cutoff, even at the lowest voltage tested.
- [ ] Voltage-based `engine_running` detection is **off by default**
      (`engineRunVoltageDetectionEnabled`, Diagnostics screen) — with it
      off, raising voltage past `engine_run_mv` must NOT make
      `engine_running` become true. This is the actual bug this toggle
      exists to fix: without it, a booster pack or this bench PSU alone
      would read as "engine running" and the starter test in §6 would find
      itself falsely refused.
- [ ] Turn the toggle on and confirm raising voltage into `engine_run_mv`
      now does make `engine_running` become true (visible via the
      diagnostics config / starter-inhibited behavior in §6), and that it
      clears again below `engine_run_mv - engine_run_hysteresis_mv`. Turn
      it back off afterward before continuing to §6, so the rest of the
      starter checklist reflects the real default a rider ships with.

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
- [ ] With `engine_running` true (§4's voltage-based detection with the
      toggle explicitly enabled, or the actual engine running once
      installed), confirm the starter button no longer fires the output.
- [ ] **Ignition-off override.** Assign an output channel as `is_ignition`
      and leave voltage-based detection off (the shipping default). With
      the ignition channel OFF, confirm `engine_running` reads false no
      matter how high the bench PSU is driven — this is the fix for the
      booster-pack/jump-start false positive, and it must hold regardless
      of the voltage toggle. Turn the ignition channel ON and confirm the
      starter button now fires normally (with the engine genuinely not
      running).
- [ ] With a neutral/clutch interlock input configured and open/unsatisfied,
      confirm the starter button is inhibited.

## 7. BLE pairing, bonding, auth

Real radio — QEMU cannot validate any of this:

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

### 7.1 Background reconnect (`src/ble/BoardSession.ts`)

The point of this feature: walk up to the bike with a paired phone in your
pocket, screen off, app not open, and have it connect and unlock on its
own. Everything below has been typechecked, compiled, and unit-tested
(`src/__tests__/BoardSession.test.ts`) but **never exercised against a real
phone** — this is exactly the kind of thing that only proves itself across
a genuine suspend/kill/relaunch cycle on real hardware, not in a simulator
or an emulator.

- [ ] **App open, board comes into range** — pair normally, background the
      app (home button, don't force-quit), walk out of range and back in.
      Reconnects and re-authenticates on its own (watch for
      `BleWatchService`'s notification on Android; nothing user-visible to
      check on iOS beyond the app showing Dashboard when reopened).
- [ ] **App backgrounded (not killed), phone locked.** Same as above with
      the screen off the whole time. iOS: this is the case
      `bluetooth-central`/state restoration is *least* likely to have
      trouble with (app process is merely suspended, not terminated).
- [ ] **iOS: app fully terminated** (swiped away in the app switcher, not
      just backgrounded), then walk into range. This is the real test of
      `restoreStateIdentifier`/`restoreStateFunction`
      (`src/ble/bleManager.ts`) — iOS has to relaunch the app in the
      background for a BLE event with no user interaction at all. Confirm
      the phone actually authenticates (check the board's event log or the
      app, once reopened, for a fresh session) — don't take "nothing crashed"
      as success.
- [ ] **Android: app backgrounded, notification visible.** Confirm
      `BleWatchService`'s notification appears (watching → connecting →
      "Connected to `<name>`" as the state changes) and that swiping it away
      doesn't kill the service (it's `setOngoing(true)` — it shouldn't be
      dismissible while running at all; if it *can* be swiped away, that's a
      bug to fix, not expected behavior).
- [ ] **Android: app force-stopped from Settings, or killed by the OS under
      memory pressure.** `START_STICKY` should get the service (and the
      process) recreated, but confirm — this is a real edge Android permits
      OEM battery-optimization features to break in ways this project can't
      control for (see the manual "disable battery optimization for this
      app" step some OEMs require, worth documenting in `docs/WIRING.md` or
      `docs/FAQ.md` if you find your device needs it).
- [ ] **Explicit Disconnect actually disconnects.** From Settings, tap
      Disconnect, confirm the board drops (BLE disconnects, and on Android
      `BleWatchService`'s notification disappears) and does **not**
      silently reconnect on its own afterward — walk away and back into
      range and confirm it stays disconnected until you reopen the app.
- [ ] **Auth failure retry timing.** Revoke this phone's key from another
      paired phone while this one is connected, confirm it gets kicked and
      then retries reconnecting on a visibly slower cadence than a plain
      out-of-range retry (30s vs 5s, `src/ble/BoardSession.ts`'s
      `AUTH_ERROR_RETRY_MS`/`CONNECT_RETRY_MS`) rather than hammering the
      radio.
- [ ] **Multiple boards.** If you own more than one, confirm pairing a
      second board (PairingScreen's manual tap flow) supersedes watching for
      the first rather than trying to watch both — this project's phone-as-key
      model is one phone, one board at a time, `saveLastDevice()` overwrites.

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
- [ ] Attempt an OTA update while `engine_running` is true (enable
      voltage-based detection for this check, per §4, or have the actual
      engine running with ignition on — off-by-default detection alone
      will not produce this state) or battery below the low-voltage
      cutoff, and confirm it's refused (`docs/PROTOCOL.md` §10.3) rather
      than silently proceeding.
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
- [ ] Decide, and record here, whether this board ships with voltage-based
      `engine_running` detection on or off (default: off). If on: start
      the engine and confirm charging voltage is correctly detected as
      `engine_running` (§4/§6) and the starter is inhibited while running.
      If off: confirm instead that turning the ignition off still
      immediately inhibits the starter (the unconditional override, §6) —
      this is the only automatic protection active with detection off, and
      it depends entirely on an `is_ignition` channel actually being
      assigned and wired.
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
