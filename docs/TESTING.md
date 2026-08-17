# MOTO-CTRL Testing & Pre-Hardware Validation

This document describes how MOTO-CTRL is tested before a physical board
exists, and how to keep testing it once one does. It's the companion to
[`docs/PROTOCOL.md`](PROTOCOL.md) (the real wire protocol). If code and
this document ever disagree, that's a bug — please report it.

The pre-hardware validation harness (sim debug GUI, QEMU boot validation)
lets the app, the lock/immobilizer system, and diagnostics (current-sense,
faults, engine-running/low-voltage cutoff) all be developed and tested with
no board on the desk. Injected faults drive the real diagnostics logic, not
a canned fault mask — see §3.1.

## 1. The test pyramid

| Layer | What it runs | What it validates | What it can't validate |
|---|---|---|---|
| Host unit tests (`ctest`, `firmware/sim/tests/`) | Portable core (`mc_*`) compiled for the host | State machines, protocol framing, crypto, config serialization — in isolation | Anything ESP-IDF-specific, timing on real hardware, BLE |
| Node integration tests (`firmware/sim/itest/`) | The sim binary + a real WebSocket client | Full protocol round-trips, cross-language Ed25519 (JS signs, C verifies) | ESP-IDF/BLE specifics |
| **Sim debug GUI** (`firmware/sim/gui/`) | The sim binary + a browser | Everything above, interactively, plus fault injection (§3) and multi-step scenarios (§4) | ESP-IDF/BLE specifics |
| **QEMU boot validation** (§5) | The real cross-compiled firmware binary, emulated | Boot sequence, NVS load, config/output restore, watchdog init — on the *actual target code*, when it boots at all (see §5's CI note) | **BLE/NimBLE radio behavior** — QEMU's Xtensa target has no BLE controller emulation |
| Bench hardware (`docs/HARDWARE_TESTING.md`) | Real board | Everything, including BLE, real GPIO, real current sensing | — |

No layer above the bottom one is a substitute for bench validation before
anything ships. The harness's payoff is that the app and the lock system
can be built and iterated on entirely against the sim, with a GUI good
enough to drive by hand and a scenario format good enough to replay a
specific bug by machine.

## 2. Running the sim + GUI locally

```sh
cd firmware/sim
cmake -S . -B build && cmake --build build
./build/moto_ctrl_sim 8010          # ws://127.0.0.1:8010
```

Then open `firmware/sim/gui/index.html` directly in a browser (no server, no
build step — it's static files; a `file://` URL works, since the page only
opens a WebSocket to the address you type, no other network access). Click
**Connect**. The default URL matches the sim's default port.

The GUI is a single static page (`index.html` + `app.js` + `style.css`) plus
a vendored copy of TweetNaCl.js for Ed25519 (`gui/vendor/`, see
`gui/vendor/README.md` — public domain, no CDN, no build step, consistent
with the project's no-cloud rule applying to dev tools too).

## 3. The sim-only debug channel

The GUI needs to do things no real client ever could: fake a battery
voltage, fake per-channel current, force a disconnect or a reboot. These go
over **`SIM_CH_DEBUG`** (byte `0x7F`), defined in
`firmware/sim/src/sim_protocol.h` — deliberately a *separate* header from
`mc_protocol.h`, so this never leaks into the real protocol third-party
clients implement against, and is never sent over real BLE. Framing matches
the real protocol: `[0x7F][opcode][payload]` on the same WebSocket
transport.

| Opcode | Direction | Payload | Effect |
|---|---|---|---|
| `SET_BATTERY_MV` (0x01) | → sim | `mv:u16le` | Injects the raw battery-sense reading; `mc_diag` calibrates and derives `battery_mv`, `engine_running`, and the low-voltage cutoff from it every tick (§3.1) |
| `SET_CHANNEL_FAULT` (0x02) | → sim | `channel:1 current_ma:u16le fault:1` | Injects a channel's raw current-sense reading; `mc_diag` calibrates and classifies the real fault from it (§3.1). The trailing `fault` byte is accepted for wire compatibility but **ignored** — forcing a fault independent of current would show a fault `mc_diag` itself never derived |
| `SET_ENGINE_RUNNING` (0x03) | → sim | `running:1` | Doesn't set the flag directly (mc_diag would overwrite it on the very next tick) — nudges the injected battery voltage to a value that makes the real derivation land on the requested state instead (§3.1) |
| `SET_INTERLOCK` (0x04) | → sim | `engaged:1` | Calls the real `mc_output_set_interlock_engaged()` |
| `BUTTON_STATE` (0x05) | → sim | `button:1 pressed:1` | Sets a virtual button's raw state, sampled by the sim's 10ms `mc_input_poll()` tick |
| `FORCE_DISCONNECT` (0x06) | → sim | — | Drops the WebSocket (simulated BLE disconnect) |
| `FORCE_REBOOT` (0x07) | → sim | — | Simulated MCU reset (§3.2), then drops the connection |
| `FORCE_NVS_CORRUPT` (0x08) | → sim | `target:1` (0=config,1=keystore,2=both,3=lock,4=calibration) | Flips bytes in the fake NVS blob; only observable on the next reboot |
| `GET_LOG` (0x09) | → sim | — | Replays the event log ring buffer (128 entries) |
| `GET_STATE` (0x0A) | → sim | — | Request a `STATE` snapshot |
| `RESET_FAULTS` (0x0B) | → sim | — | Resets injected battery/current back to defaults (mc_diag then re-derives fault/cutoff/engine_running from those on its own next tick); clears the interlock override too (does not touch lock state — that's not a "fault") |
| `ACK` (0x81) | ← sim | `req_op:1 ok:1` | Acknowledges a setter |
| `LOG_ENTRY` (0x90) | ← sim | `t_ms:u32le text_len:1 text` | One event, pushed live and on `GET_LOG` replay |
| `STATE` (0x91) | ← sim | `battery_mv:u16le running:1 interlock:1` then 12×`[current_ma:u16le fault:1]` then `lock_state:1 cheatcode_backoff:1` | `battery_mv`, `running`, `current_ma`, and `fault` are the **real `mc_diag`-computed values** (round-tripped through its calibration and threshold logic), not raw injected numbers — the same values `STATUS` reports over the real protocol, never out of sync with it |

### Lock/immobilizer note: no new debug opcodes

The lock/immobilizer system needed **no additions to `SIM_CH_DEBUG`** beyond
the `STATE` snapshot fields and the `FORCE_NVS_CORRUPT` lock target above.
The reason: every lock-affecting input the GUI needs to drive was already
covered —

- **Phone-as-key** and **cheat-code entry** go over the *real* protocol
  (`docs/PROTOCOL.md` §11's new COMMAND-channel opcodes), driven by the
  GUI's existing Auth panel and virtual buttons — not the debug channel at
  all.
- **Ignition-switch mode** reuses one of the 8 existing virtual buttons
  (`mc_lock_config_t.ignition_switch_input` is an input index, exactly like
  `starter_interlock_input`) — assign it via `MC_OP_LOCK_SET_CONFIG`, then
  hold that virtual button (mousedown) to simulate the switch being ON. A
  held virtual button already IS a maintained switch's debounced level; no
  separate simulated signal was needed.
- **Engine-running** was already simulated (`SET_ENGINE_RUNNING`) and
  mc_lock's parked/lock guards consume the same flag starter protection
  already used.

### Diagnostics note: no new debug opcodes

Diagnostics needed **no additions to `SIM_CH_DEBUG`** either — only a
semantic change to two existing opcodes (`SET_CHANNEL_FAULT`,
`SET_ENGINE_RUNNING`, both documented in the table above) and one new
`FORCE_NVS_CORRUPT` target (`4` = calibration blob). The reasons:

- **Current-sense.** `SET_CHANNEL_FAULT`'s `current_ma` field already
  existed as a pure display value; diagnostics wires it into `mc_diag`'s
  real HAL as the raw reading, so the same injection point now feeds real
  calibration + threshold classification instead of a hand-picked fault
  enum.
- **Battery / engine-running / low-voltage cutoff.** `SET_BATTERY_MV`
  already existed too; `mc_diag` derives everything else from it every
  tick. `SET_ENGINE_RUNNING` survives only as sugar that nudges the battery
  voltage to a value producing the requested state — see the table above.
- **Calibration** has no debug op at all: it's exercised through the
  **real** protocol (`DIAG_GET_CALIB`/`DIAG_SET_CALIB`, `docs/PROTOCOL.md`
  §12), same as lock config uses real ops rather than debug ones.

### Flashers/PWM note: no new debug opcodes

Flashers/PWM needed **no additions to `SIM_CH_DEBUG`** at all — every input
is already reachable over the *real* protocol: `SET_OUTPUT` and
`HAZARD_PRESS` (`docs/PROTOCOL.md` §13) for turn/hazard control, the config
JSON for mode/duty/timing/`brake_switch_input`, and the existing virtual
buttons + `mc_input_button_level()` for the brake-switch input itself (same
mechanism the ignition-switch input already uses, per the note above).

The one new question this raised: how do tests avoid waiting out a real
30-second auto-cancel timer or a 700ms blink period against the sim's real
wall clock? **Not** a time-warp debug opcode — tests just write a short
test-specific value (e.g. `turn_auto_cancel_ms: 300`) through the real
config-write path first, then sleep a real (short) interval and assert.
This is deliberately the same "use the real protocol to make the real
timing fast enough to test" approach the diagnostics `DIAG_SETTLE_MS`
sleeps already use, rather than adding sim-only time control that would
diverge from what a real client can actually do. Blink-phase-accurate
timing itself (exact on/off transitions at known millisecond offsets) is
not itest's job at all — that's `firmware/sim/tests/test_output.c`,
against synthetic time, deterministically.

### 3.1 Fault injection and the status wire

Early on, `mc_status_t.output_fault_mask` (`docs/PROTOCOL.md` §5) was a
reserved wire field that the **simulator** populated directly from
whatever fault enum the GUI injected — no ADC or threshold comparison
involved, only whatever the debug channel was told to report.

**Diagnostics changes this for real.** `mc_diag`
(`firmware/components/core/mc_diag.c`) now owns `output_fault_mask`,
`battery_mv`, `engine_running`, and the low-voltage cutoff — the same
compiled code that ships on-target, proven by the host `ctest` suite
(`test_diag.c`) and exercised end-to-end against a real spawned sim process
(`firmware/sim/itest/integration.test.mjs` and
`app/src/__tests__/sim.itest.test.js`). The sim's role shrinks to exactly
one thing: its
`mc_diag_hal_t` reads back `SET_BATTERY_MV`/`SET_CHANNEL_FAULT`'s injected
raw values instead of driving a real ADC — everything downstream of that
(calibration math, round-robin sampling, open-load/overcurrent
classification, engine_running/cutoff derivation with hysteresis) is the
real firmware logic running unmodified. The sim's *default* calibration is
deliberately an **identity mapping** (`firmware/sim/src/sim_nvs.h`'s doc
comment) so an injected reading shows up as the same number by default —
a test that explicitly calls `DIAG_SET_CALIB` still changes the reported
values, exactly like real hardware would.

**What's still not validated here**: real analog behavior — actual PROFET
`kILIS` accuracy, ADC offset/gain drift, DSEL/DEN mux settle timing, IS-line
crosstalk between channels, and the real charging-system voltage profile.
Those remain bench-only (`docs/HARDWARE_TESTING.md`); this layer proves the
portable logic is correct, not that the physical sense lines read true.

### 3.2 Simulated reboot / watchdog trip

`FORCE_REBOOT` reloads `mc_config_t` and `mc_keystore_t` from the sim's fake
NVS (`firmware/sim/src/sim_nvs.c`) through the **same** `mc_config_load()` /
`mc_keystore_deserialize()` code paths real firmware uses on a real reboot —
including the same fail-safe-to-defaults behavior on a corrupt or missing
blob (never abort — `CONTRIBUTING.md` safety requirement #1). It then calls
the real `mc_output_restore_from_config()`, timed against that same
requirement's **<250ms** restore budget, and logs the result (`PASS`/`FAIL`)
to the event log before
dropping the connection — a real reboot drops the BLE link too. Observed
restore times in testing are in the tens-to-low-hundreds of *microseconds*
(no flash access in the sim), so the budget is not a meaningful stress test
of the sim itself — it's meaningful once the equivalent QEMU/bench timing is
measured against real NVS flash latency.

The sim's debounced persistence (`mc_persist`, 2000ms, same as real
firmware) is **not** flushed before a forced reboot — a state change inside
the debounce window is genuinely lost, exactly like a real reboot would lose
it. This is a deliberate, realistic test case: toggle an output, immediately
force a reboot, and confirm the restored state reflects the *last flushed*
value, not the most recent command.

`FORCE_REBOOT` also reloads the lock config + `locked_flag` from the fake
NVS and re-runs `mc_lock_init()`, inside the same timed window — a
persisted `LOCKED` state restoring (with the ignition/starter immobilize
re-applied) is covered by the same 250ms budget check as the output
restore. Unlike config/keystore, lock/cheat-code changes are **never
debounced** (see `mc_lock.h`) — they persist immediately on every mutating
command, so there is no "lost inside the debounce window" case to test for
lock state specifically.

It also reloads board calibration from its own fake-NVS blob and re-runs
`mc_diag_init()` (also inside the timed window), resetting `mc_diag`'s
runtime state — current-sense history, `engine_running`, and cutoff status
— cleanly, same as a real reboot: current-sense history from before a
watchdog reset isn't meaningful to carry forward. Diagnostics *threshold*
config (open-load/overcurrent/cutoff/engine-run) rides `mc_config_t` and so
reloads as part of the same config reload the output restore already
depends on — no separate reload path needed for it. Calibration, like lock
state, persists **immediately** on `DIAG_SET_CALIB` rather than through the
debounced scheduler (a rare, deliberate installer/bench action, not a hot
path).

`mc_output_init()` (called fresh on every `FORCE_REBOOT`, same as a real
boot) zeroes the whole engine, including the per-channel
`turn_auto_cancel_deadline_ms[]`/`brake_burst_started_ms[]` runtime arrays —
a pending auto-cancel timer or an in-progress brake-flasher burst does not
survive a reboot, same "not meaningful to carry forward" doctrine as the
current-sense history above. Mode/duty/timing config rides
`mc_config_t.outputs` and so reloads through the existing config reload
path — no separate reload code needed for it, same as diagnostics
thresholds above.

## 4. Scenario recording & replay

The GUI's **Scenario recorder** panel records every action a human (or a
loaded scenario) triggers, with real relative timestamps, into a flat JSON
action list:

```json
{ "version": 1, "actions": [
  { "t": 0,    "name": "setBattery", "args": { "mv": 11500 } },
  { "t": 1200, "name": "buttonState", "args": { "button": 2, "pressed": true } },
  { "t": 1350, "name": "buttonState", "args": { "button": 2, "pressed": false } }
] }
```

`t` is milliseconds since recording started. `name` is a key into the GUI's
own action table (`ACTIONS` in `app.js` — the same table both live UI
buttons and scenario replay call through, so recording can never drift from
what replay does). Loading a scenario re-schedules every action at its
recorded offset via `setTimeout`, so e.g. "wrong cheat code 3 times while
battery is low" is captured once by hand and replayed byte-for-byte
thereafter, including timing (useful for combo/cheat-code window edge
cases, which are timing-sensitive by definition).

Scenario files are plain JSON, not committed anywhere by default — save the
ones worth keeping under a location of your choosing (e.g.
`firmware/sim/gui/scenarios/`, not yet created) as regression fixtures.

## 5. QEMU boot validation

**Not run in CI as of 2026-08.** This was originally a `firmware-qemu` job
in `.github/workflows/firmware.yml` that built the real on-target firmware,
booted it under ESP-IDF's bundled QEMU, and grepped the serial log for the
boot-sequence markers below. It was removed after the job started hanging
for the full boot-sequence check on every run, confirmed to be an upstream
ESP-IDF/QEMU issue rather than a firmware regression:

- The hang happens *before* `app_main()` is ever called — confirmed by
  temporarily instrumenting the first line of `app_main()` with a raw
  `esp_rom_printf()`; it never printed. Nothing in this repo's code runs
  that early, so nothing in this repo caused it.
- Reproduced identically against two different ESP-IDF/QEMU combinations
  (the `espressif/idf:latest` container's bundled dev snapshot, and the
  pinned stable `espressif/idf:v6.0.2`) via `docker run` locally — same
  hang, same last log line (`W (...) eFuse: calibration efuse version does
  not match, set default version to 0`), immediately followed by silence
  until the job's own timeout killed it.
- §7 below already flagged a related, never-resolved oddity — a
  `gpio: conflict found for GPIO[3]` warning during QEMU boot — as
  QEMU-specific and worth tracing. This is very plausibly the same
  underlying conflict, now hanging outright instead of just warning under
  whatever QEMU/IDF combination CI happened to be pulling.

Given `docs/TESTING.md`'s own pyramid framing (*"No layer above the bottom
one is a substitute for bench validation"*), this was always the thinnest,
most infrastructure-fragile layer — useful as a boot-sanity signal, never
a correctness gate. Blocking every push on an unresolved upstream QEMU
issue unrelated to code changes cost more than the signal was worth, so it
was dropped from CI rather than left failing indefinitely or made
non-blocking-but-permanently-red. It's still fully runnable locally (below)
for anyone who wants to pick the GPIO3/QEMU conflict back up.

Historically (and still, locally, once/if the underlying QEMU issue is
sorted out): builds the real on-target firmware, boots it under ESP-IDF's
bundled QEMU (`idf.py qemu`, Xtensa target), and greps the serial log for
boot-sequence markers logged from `firmware/main/main.c`:

```
MOTO-CTRL boot: early_init done
MOTO-CTRL boot: NVS init done
MOTO-CTRL boot: outputs restored
MOTO-CTRL boot: lock state restored
MOTO-CTRL boot: diagnostics engine ready
MOTO-CTRL boot: input engine ready
MOTO-CTRL boot: watchdog init done
MOTO-CTRL boot: starting BLE stack
```

**This confirms the compiled binary's boot sequence, NVS read, config load
+ fallback, output restore, and input engine init all run correctly on real
(emulated) Xtensa hardware** — checks the host `ctest` suite structurally
cannot make, because it never runs through ESP-IDF's boot process, NVS
driver, or FreeRTOS scheduler at all.

**QEMU does NOT validate BLE.** Shortly after "starting BLE stack," NimBLE
controller init hits a QEMU-only assertion failure and the firmware
crash-reboot-loops — QEMU's Xtensa target has no BLE radio controller
emulation. This is expected and is not a firmware bug; the CI job only
checks for markers through "starting BLE stack" and always kills QEMU on a
timeout rather than waiting for a clean exit. **Do not read a passing QEMU
job as any evidence about pairing, bonding, GATT, or auth working on real
hardware** — that is bench-only territory (`docs/HARDWARE_TESTING.md`).
This caveat is intentionally repeated here and in the CI
step's own comments, since it is easy to misread a green QEMU job as "BLE
works."

To run it locally:

```sh
cd firmware
idf.py set-target esp32s3 && idf.py build
python "$IDF_PATH/tools/idf_tools.py" install qemu-xtensa
timeout 30 idf.py qemu   # or omit `timeout`/`30` and Ctrl-C once you've seen enough
```

## 6. Feature acceptance checklist

Status of every use case the pre-hardware validation harness covers.
"Automated" means covered by `ctest`/the Node integration suite/QEMU CI;
"GUI" means exercised interactively (or via a saved scenario) but not yet
asserted by a script; "N/A" means the underlying feature doesn't exist.

| Use case | Status | Notes |
|---|---|---|
| Pair, bond, authenticate, read status, toggle output, get notified | Automated + GUI | Node itest + GUI auth/output panels |
| Button combo → output action binding | **N/A** | The generic `combos[]` chord/sequence mechanism still has no action bound to it — out of scope for both the cheat-code (see next row) and the plain `short_press_action[]` binding (which binds directly, not through a combo — see the turn-signal row below) |
| Cheat-code entry, in/out of timing window | Automated + GUI | `mc_lock` buffers real short-press events independently of the generic combo matcher (never stores the code in plaintext — see `mc_lock.h`); `test_lock.c` covers in-window/timeout/backoff, itest drives real button taps end-to-end, GUI has a live cheat-code panel |
| Turn signal / hazard / brake-flasher timing, PWM dimming | Automated + GUI | `mc_output` has blink/pulse-pattern modes, device-side turn mutual-exclusion + auto-cancel (embedded in `mc_output_set()` itself), and `HAZARD_PRESS`. `test_output.c` covers blink-phase timing, burst patterns, mutual exclusion, auto-cancel expiry, and PWM duty dispatch deterministically against synthetic time (real timing is not itest's job — see §3's flashers/PWM note); itest (Node + app) confirm mutual exclusion/auto-cancel/hazard/config round-trip end-to-end against a real spawned sim; GUI has a Hazard button and the Config JSON panel exposes every field |
| Config export / wipe / import round-trip | Automated + GUI | Config panel; commit preserves live output state (`CONTRIBUTING.md` safety requirement #1) |
| OTA transfer (begin/chunk/commit), signature + hash verification, safe-state gating | Automated + GUI | `test_ota.c` covers the state machine in isolation (bad signature, out-of-order chunks, hash mismatch, unsafe-state rejection at both `OTA_BEGIN` and `OTA_REBOOT`); itest drives a full signed transfer against the sim's fixed TEST OTA keypair via `moto-client.mjs`'s `otaTransfer()`; GUI has begin/chunk/commit/abort/reboot/status controls (docs/PROTOCOL.md §10) |
| Event log: security/safety events recorded and readable | Automated + GUI | `test_event_log.c` covers the ring buffer (append/evict/read-since/clear) in isolation; itest confirms real events (lock, key enroll/revoke, OTA begin/success/failure, low-voltage cutoff) actually land in the log and are readable via `EVENT_LOG_GET` (docs/PROTOCOL.md §15); GUI has an event log viewer |
| Unauthenticated write → rejected | GUI (scripted check) | Dedicated test button, reads the actual result code rather than assuming |
| BLE disconnect mid-write → outputs hold state | GUI | Force Disconnect while a write is in flight; sim never changes output state on disconnect (nothing in `mc_session`/`mc_output` does either) |
| Simulated reboot/watchdog trip, restore timing | GUI, timing logged | §3.2; PASS/FAIL against the 250ms budget logged per reboot (includes lock-state restore) |
| Wrong cheat code repeated — lockout/backoff policy | Automated + GUI | 5 free attempts, then progressive backoff (15s/30s/60s cap) that gates *only* the cheat-code — phone-as-key and ignition-switch stay available throughout (`CONTRIBUTING.md` safety requirement #3). `test_lock.c` + itest cover the full progression; GUI's Status panel shows a live BACKOFF indicator |
| Low battery crossing configured cutoff → non-essential outputs disabled | Automated + GUI | `mc_diag` derives the cutoff from real (calibrated) battery voltage with hysteresis, engaging only while `!engine_running`; `mc_output_set_lv_cutoff()` suppresses every non-essential channel's HAL output while preserving `commanded_on` (`CONTRIBUTING.md` safety requirements #1/#7). `test_diag.c` + `test_output.c` cover engage/recover/hysteresis/essential-exemption; itest (both Node and app) confirm it end-to-end against a real spawned sim; GUI's Diagnostics panel + Status "LV cutoff" stat make it observable live |
| Starter request while "engine running" → rejected | Automated + GUI | `mc_output`'s starter protection is real. `engine_running` used to be only a GUI-injected stand-in (`SET_ENGINE_RUNNING` calling `mc_output_set_engine_running()` directly); **`mc_diag` now derives it for real** from calibrated battery voltage every tick, the same signal driving both the starter guard and `mc_lock`'s parked-detection guard. `SET_ENGINE_RUNNING` survives only as a convenience that nudges the injected battery voltage (§3's diagnostics note). The lock system adds the analogous immobilize check (starter *and* ignition refused while LOCKED, any source) on top |
| Blown-bulb / open-load detection, per-channel current, learnable thresholds | Automated + GUI | Round-robin current sampling (one actually-energized channel per ~10ms tick, `hardware/PINOUT.md`'s shared IS-mux constraint), classified against per-channel configurable thresholds (never hardcoded); `DIAG_LEARN` sets a threshold from a real measured sample. `test_diag.c` covers sampling/classification/learn in isolation; itest proves it end-to-end (inject current → real fault appears in `DIAG_GET` and the STATUS fault mask); GUI's Outputs panel shows the live current + a read-only fault badge, Diagnostics panel edits thresholds and has per-channel/all Learn buttons |
| Board calibration (current-sense gain/offset/kILIS, battery divider) | Automated + GUI | Own NVS blob (`mc_calib`), deliberately excluded from the exportable config *and* from factory reset/ownership transfer (describes the board, not the owner). `test_diag.c` covers serialize/deserialize + corruption; itest confirms `DIAG_SET_CALIB` actually changes subsequently reported current; GUI has a calibration section |
| Phone bonded, out of range, cheat code still works | Automated + GUI | Cheat-code entry never reads any phone/auth state at all — an in-range/authenticated phone never suppresses it (see `mc_lock.h`'s truth table). Covered by `test_lock.c`; itest's backoff test explicitly demonstrates phone unlock working while the cheat-code itself is in backoff |
| Two phones bonded, both authenticate; revoke one | Automated + GUI | Multi-key enroll/authenticate/revoke flow, itest + GUI |
| Corrupted/unexpected NVS config on boot → safe fallback | Automated (host `test_config.c`) + GUI (§3.2) + QEMU (real NVS path, no corruption injection there yet) | The same fail-safe-to-disabled/unlocked fallback covers a corrupt lock blob (`nvs_lock_hal.c` / `sim_nvs_lock_load`) and a corrupt calibration blob (`nvs_calib_hal.c` / `sim_nvs_calib_load`, falling back to nominal/uncalibrated defaults — never a hard failure) |
| Immobilizer locks only when parked; never while ignition live | Automated | `mc_lock_request_lock()` and the auto-lock grace timer both hard-guard on `!engine_running && !ignition_live` (`CONTRIBUTING.md` safety requirement #2) — `test_lock.c` covers the guard, the auto-lock path, and the boot-time ride-safe override (never restore into LOCKED while the engine appears to be running) |
| Physical factory reset (hold BOOT 10s) wipes bonds + lock config | GUI/manual only (real GPIO, not simulated) | `firmware/main/factory_reset.c` — no sim equivalent, since the sim has no BOOT-pin analog; covered on hardware only (`docs/HARDWARE_TESTING.md`) |

## 7. Other findings surfaced by this harness

- **GPIO3 conflict warning under QEMU boot, now escalated to a full boot
  hang (see §5)**: the serial log used to show
  `W (nnn) gpio: conflict found for GPIO[3]` during early boot without
  otherwise stopping the boot sequence. As of 2026-08, `idf.py qemu` no
  longer completes the boot at all — it hangs before `app_main()` is ever
  called, on both the `espressif/idf:latest` dev snapshot and the pinned
  stable `v6.0.2`, which is what got `firmware-qemu` dropped from CI (§5).
  GPIO3 is one of the two strapping pins (`hardware/PINOUT.md`) with
  special handling requirements in `board_config_early_init()` — but since
  the hang happens before that function (or any of this repo's code) runs,
  this repo's GPIO3 handling isn't itself the cause; the conflict, if
  that's really what it is, is between QEMU's own console/pin setup and
  something in ESP-IDF's pre-`app_main` startup. Diagnostics does **not**
  touch GPIO3/PROFET_IN6 at all — `diag_hal.c` only configures the shared
  `DSEL` (GPIO48), the per-device `DEN` lines (including GPIO46, the
  *other* strapping pin, U4's DEN), and the two ADC channels; GPIO3 stays
  owned exclusively by `output_hal_gpio.c`. Still unresolved, still worth a
  look for anyone who wants to bisect ESP-IDF/qemu-xtensa versions or dig
  into Espressif's QEMU fork directly — flagged again rather than silently
  dropped.
- The host simulator's `mc_input` engine runs a real 10ms-tick
  `mc_input_poll()` loop so the GUI's virtual buttons exercise the actual
  debounce/combo engine, not a stand-in.
- The sim's `persist_config`/`persist_keystore` are backed by an *opt-in*
  fake NVS (`sim_nvs.c`) so `FORCE_REBOOT` and `FORCE_NVS_CORRUPT` have
  something real to load from — CI's own `moto_ctrl_sim` process still
  starts with empty fake NVS every run, so the Node integration suite stays
  hermetic (nothing persists across a process restart, only across an
  in-process `FORCE_REBOOT`).
- **Two real bugs were found and fixed during development, worth recording
  since they were genuine pre-existing gaps rather than anything caught by
  design review:**
  1. `mc_output_set()` had always overwritten a channel's `mode` to mirror
     the commanded on/off state on every call (harmless as long as `mode`
     was never independently meaningful) — this would have silently
     discarded any `flash_turn`/`pwm`/`flash_brake` mode choice on the very
     next `SET_OUTPUT`. Fixed by making `mode` a real, independent,
     persisted property (`mc_output_config_default()`'s default changed
     `off`→`on` accordingly — see its comment). Regression-covered by
     `test_config_validate_flags_bad_pwm_duty` and friends in
     `test_output.c`, and by
     `test_config_write_preserves_imported_mode_not_just_on_off` in
     `test_session.c`.
  2. `mc_output_set()`-driven commanded_on changes were **never actually
     persisted** — `firmware/main/main.c`'s debounced config flush always
     saved `s_config`, but nothing ever copied `s_output.config` (the live
     truth, a separate copy since `mc_output_init()`) back into it outside
     of a full config-JSON commit. A plain `SET_OUTPUT` would work live but
     silently fail to survive a reboot. Fixed with a single diff-and-sync
     check in `main.c`'s/`sim/src/main.c`'s tick loop (catches BLE-driven,
     local-button-driven, and auto-cancel-timer-driven changes uniformly —
     see the comment at that check) rather than scattering a persist call
     at every mutation site. This was a long-standing gap that only became
     impossible to ignore once enough new local (non-BLE) mutation call
     sites existed to make the missing sync obvious while reasoning through
     the design.
