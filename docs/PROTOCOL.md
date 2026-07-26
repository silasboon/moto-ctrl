# MOTO-CTRL BLE Protocol

This document specifies the MOTO-CTRL wire protocol well enough for a
third-party client to be built. It is the authoritative companion to the
implementation in `firmware/components/core/` (`mc_protocol.h`,
`mc_session.c`) and the GATT definitions in `firmware/main/ble/`. If code and
this document ever disagree, that is a bug — please report it.

> **Status:** the status, control, config, lock/immobilizer (§11),
> diagnostics (§12), flashers/PWM (§13), OTA (§10), and event log (§15)
> services are all implemented.

## 1. Transports

The same logical protocol runs over two transports:

- **BLE GATT** (real hardware). Each *channel* below is a GATT
  characteristic. A client writes a frame to a characteristic and receives
  responses as notifications on the same characteristic.
- **WebSocket** (the `firmware/sim/` host simulator, for development and CI
  with no hardware). Each message is a single binary WebSocket frame whose
  **first byte is the channel id**, followed by the frame bytes. The
  simulator listens on `ws://127.0.0.1:<port>` (default `8010`).

A "frame" is identical on both transports:

```
frame = opcode:u8  payload:bytes
```

On BLE the channel is implied by which characteristic carried the frame; on
WebSocket the channel is the leading byte. Everything after that — opcode and
payload — is the same. A worked reference client is
`firmware/sim/itest/moto-client.mjs`.

Multi-byte integers are **little-endian** unless stated otherwise.

## 2. GATT layout

Base UUID: `5a4f00XX-9b1e-4f8a-9c2d-1a2b3c4d5e6f`, where `XX` selects the
service or characteristic.

| Service (UUID `XX`) | Characteristic (UUID `XX`) | Channel id | Properties |
|---|---|---|---|
| Status `0x10` | Status `0x11` | `0` | Read, Write, Notify |
| Control `0x20` | Auth `0x21` | `1` | Write, Notify |
| Control `0x20` | Command `0x22` | `2` | Write, Notify |
| Config `0x30` | Config `0x31` | `3` | Write, Notify |
| OTA `0x40` | OTA `0x41` | `4` | Write, Notify |

The device advertises as **`MOTO-CTRL`**; the Status service UUID is in the
scan response.

Clients that will read config **must** negotiate an ATT MTU large enough to
carry a config chunk frame (see §7): the 128-byte chunk payload plus a 4-byte
chunk header, opcode, and ATT overhead — request an MTU of at least ~185.

Clients that will perform an OTA update should negotiate the largest MTU the
stack supports (BLE 5's practical ceiling is 512 bytes of ATT payload) — a
larger MTU means fewer `OTA_CHUNK` round trips for the same image. There is
no minimum: `OTA_CHUNK` accepts any chunk size that fits one write, down to a
single byte, at the cost of more round trips.

## 3. Pairing and session model

1. **BLE bonding.** The client pairs using **LE Secure Connections**
   (Just Works; bonding enabled). This encrypts the link and persists a bond.
   Link/bond alone grants **no** authority — see below.
2. **Application-layer authentication.** Every connection starts an
   unauthenticated *session*. Until the session is authenticated via the
   challenge-response (§5), the device permits only: reading status (§4),
   `AUTH` operations (§5), and *first-key* enrollment (§6). All
   state-changing operations (control, config write) are refused with
   `UNAUTHENTICATED`.
3. A session is per-connection. Disconnecting drops the authentication;
   reconnecting requires authenticating again.

MAC address is never trusted as identity (see `CONTRIBUTING.md`'s safety
requirement #4).

## 4. Result codes

`*_RESULT` payloads carry a one-byte result:

| Value | Name | Meaning |
|---|---|---|
| 0 | `OK` | success |
| 1 | `UNAUTHENTICATED` | session has not passed challenge-response |
| 2 | `BAD_REQUEST` | malformed or truncated payload |
| 3 | `REJECTED` | semantically refused (e.g. starter over BLE, invalid config) |
| 4 | `ENROLL_DENIED` | enrollment not permitted in the current state |
| 5 | `KEYSTORE_FULL` | no free key slot |
| 6 | `NOT_FOUND` | referenced key slot not present |
| 7 | `NOT_IMPLEMENTED` | unused (OTA is implemented); kept for wire stability |
| 8 | `INTERNAL` | device-side error (e.g. RNG failure) |

## 5. Status channel (`0`)

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x01` `STATUS_GET` | — |
| ← device | `0x81` `STATUS` | 16-byte snapshot (below) |

On BLE the status characteristic is also directly **readable** (ATT Read),
returning the same 16-byte snapshot without a write. Status is available to
unauthenticated sessions.

Snapshot layout (16 bytes, little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | firmware major |
| 1 | 1 | firmware minor |
| 2 | 1 | firmware patch |
| 3 | 1 | lock state (`0`=UNKNOWN,`1`=PARKED,`2`=LOCKED,`3`=UNLOCKED) — see §11 |
| 4 | 4 | uptime (ms) |
| 8 | 2 | battery (mV) — calibrated reading, §12 |
| 10 | 2 | output state mask — bit *c* set ⇒ channel *c* is **commanded** on (rider/app intent — see §12's note on why this is not the same as "actually driven") |
| 12 | 2 | output fault mask — bit *c* set ⇒ channel *c* has a real open-load/overcurrent fault, §12 |
| 14 | 1 | RSSI (dBm, signed) — `0` if unknown |
| 15 | 1 | bit 0: cheat-code entry is in backoff (§11.4); bit 1: low-voltage cutoff is active (§12); bits 2–7 reserved |

## 6. Auth channel (`1`)

### Challenge-response

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x01` `AUTH_BEGIN` | — |
| ← device | `0x81` `AUTH_CHALLENGE` | `nonce` (32 bytes) |
| → device | `0x02` `AUTH_RESPONSE` | `signature` (64 bytes) |
| ← device | `0x82` `AUTH_RESULT` | `result:u8` `slot:u8` |

The client signs, with its Ed25519 **private key**, the message:

```
auth_message = "moto-ctrl-auth-v1"  ‖  nonce            (17 + 32 = 49 bytes)
signature    = Ed25519_sign(private_key, auth_message)  (detached, 64 bytes)
```

The 17-byte ASCII prefix is domain separation; there is no trailing NUL. The
device verifies the detached signature against every enrolled public key; a
match authenticates the session and returns the matching key `slot`. The
nonce is single-use and freshly random per `AUTH_BEGIN`, so a captured
signature cannot be replayed on another session. On failure the session
returns to unauthenticated and a new `AUTH_BEGIN` (new nonce) is required.

Ed25519 here is standard (RFC 8032, SHA-512), interoperable with
`tweetnacl` / `tweetnacl-js` (`nacl.sign.keyPair`, `nacl.sign.detached`).

### Enrollment

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x10` `ENROLL` | `pubkey` (32 bytes) `label` (UTF-8, ≤23 bytes) |
| ← device | `0x90` `ENROLL_RESULT` | `result:u8` `slot:u8` |

Enrollment is permitted when **either** the keystore is empty
(trust-on-first-use for the first key on a fresh or factory-reset device)
**or** the session is already authenticated (an existing owner adding another
phone). Otherwise it returns `ENROLL_DENIED`. Up to 8 keys may be enrolled.
Enrolling an already-present public key updates its label (idempotent).

> This forward-reference (written before the immobilizer existed) anticipated
> adding physical-button confirmation for the unattended-bike case on top of
> this rule. The immobilizer's actual implemented scope turned out to be the
> lock state machine, cheat-code, and ignition-switch mode specifically —
> enrollment confirmation wasn't part of it, and the existing TOFU/
> authenticated-owner rule above is unchanged. Flagged as an open item
> rather than silently implemented or silently left inaccurate.

### Key management (authenticated)

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x11` `KEY_LIST` | — |
| ← device | `0x91` `KEY_LIST_RESULT` | `count:u8` then per key: `slot:u8` `label_len:u8` `label:bytes` |
| → device | `0x12` `KEY_REVOKE` | `slot:u8` |
| ← device | `0x92` `KEY_REVOKE_RESULT` | `result:u8` `slot:u8` |

Revoking a key removes its public key immediately. Wiping all keys (ownership
transfer) is done via factory reset or by revoking each key.

## 7. Command channel (`2`) — authenticated

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x01` `SET_OUTPUT` | `channel:u8` `on:u8` |
| ← device | `0x81` `COMMAND_RESULT` | `req_opcode:u8` `result:u8` |

`SET_OUTPUT` switches an output channel (0–11). It is refused with
`REJECTED` if the channel's assigned function is **starter** — the starter is
never triggerable over the wire (see `CONTRIBUTING.md`'s safety requirement
#6), only via the hardware button path. Unauthenticated sessions get
`UNAUTHENTICATED`.

For a channel whose assigned function is `turn_l`/`turn_r`, `SET_OUTPUT`
also applies turn-signal policy device-side — see §13.

## 8. Config channel (`3`) — authenticated

Config is exchanged as JSON (see §9), chunked because a full config exceeds a
single BLE notification.

### Read

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x01` `CONFIG_READ` | — |
| ← device | `0x81` `CONFIG_CHUNK` | `offset:u16` `total:u16` `data:bytes` |

The device streams `CONFIG_CHUNK` frames (≤128 data bytes each). The client
reassembles by `offset` until it has `total` bytes.

### Write

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x02` `CONFIG_WRITE_BEGIN` | `total:u16` |
| → device | `0x03` `CONFIG_WRITE_CHUNK` | `offset:u16` `data:bytes` |
| → device | `0x04` `CONFIG_WRITE_COMMIT` | — |
| ← device | `0x82` `CONFIG_WRITE_RESULT` | `result:u8` |

`WRITE_BEGIN` declares the total JSON length (≤4096; larger is
`BAD_REQUEST`). The client sends the JSON as `WRITE_CHUNK`s, then `COMMIT`.
On commit the device parses the JSON, validates it (e.g. at most one
ignition, at most one starter — otherwise `REJECTED`), applies the new
configuration, and persists it. **Live output states are preserved across a
config write** — importing a config never toggles outputs (see
`CONTRIBUTING.md`'s safety requirement #1). `WRITE_BEGIN`/`WRITE_CHUNK` are
acknowledged only on error; `COMMIT` always returns a result.

## 9. Config JSON

The interchange form of the configuration. Fields absent on import take their
defaults; unknown `function` strings map to `"none"`.

```json
{
  "schema_version": 3,
  "outputs": {
    "channels": [
      { "function": "headlight_lo", "name": "Low Beam", "mode": "on", "pwm_duty_pct": 100, "commanded_on": false }
    ],
    "starter_interlock_input": -1,
    "brake_switch_input": -1,
    "turn_auto_cancel_ms": 30000,
    "turn_flash_period_ms": 700,
    "brake_flash_pulse_count": 3,
    "brake_flash_pulse_on_ms": 150,
    "brake_flash_pulse_off_ms": 50
  },
  "inputs": {
    "timing": { "debounce_ms": 20, "long_press_ms": 600, "double_press_gap_ms": 350 },
    "combos": [
      { "type": "sequence", "buttons": [0, 1, 0, 1], "window_ms": 5000, "action_id": 42 }
    ],
    "short_press_action": [0, 0, 0, 0, 0, 0, 0, 0],
    "long_press_action": [0, 0, 0, 0, 0, 0, 0, 0],
    "double_press_action": [0, 0, 0, 0, 0, 0, 0, 0]
  },
  "diagnostics": {
    "channels": [
      { "open_load_ma": 50, "overcurrent_ma": 15000 }
    ],
    "lv_cutoff_mv": 11800,
    "lv_cutoff_hysteresis_mv": 300,
    "engine_run_mv": 13800,
    "engine_run_hysteresis_mv": 300
  }
}
```

- `outputs.channels` has exactly 12 entries (channels 0–11).
- `function` ∈ `none, headlight_hi, headlight_lo, brake, turn_l, turn_r, horn, ignition, starter, aux`.
- `mode` ∈ `off, on, pwm, flash_turn, flash_brake` (see §13). Unknown/absent
  maps to `on`, not `off` — `on` (plain digital) is the default electrical
  behavior for a freshly-assigned channel.
- `pwm_duty_pct`: 1–100, meaningful only when `mode` is `pwm`.
- `starter_interlock_input` is an input index (0–7) or `-1` for none.
- `brake_switch_input`: an input index (0–7) or `-1` for none — see §13.
- `turn_auto_cancel_ms`, `turn_flash_period_ms`, `brake_flash_pulse_count`,
  `brake_flash_pulse_on_ms`, `brake_flash_pulse_off_ms`: see §13.
- `combos[].type` ∈ `chord, sequence`; `buttons` are input indices (0–7).
- `diagnostics.channels` has exactly 12 entries (channels 0–11) — per-channel
  open-load/overcurrent thresholds (§12), added at `schema_version` 2.
  **Board calibration is deliberately not here** — it has its own dedicated
  ops (§12) and never rides a config export/import, since it describes one
  physical board's analog sense lines, not a portable setting.
- `schema_version` is 3 (grew `outputs` again — see above).

## 10. OTA channel (`4`) — authenticated

Firmware update over A/B partitions (`firmware/partitions.csv`'s `ota_0`/
`ota_1`), implemented in `mc_ota.c` and driven from `mc_session.c`. Every op
below requires an authenticated session, same as the COMMAND/CONFIG channels
— an unauthenticated frame gets `0x8F` `OTA_RESULT` with `UNAUTHENTICATED`
and no other effect.

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x01` `OTA_BEGIN` | `image_size:u32le` `sha512:u8[64]` `signature:u8[64]` (132 bytes) |
| → device | `0x02` `OTA_CHUNK` | `offset:u32le` `data:bytes` |
| → device | `0x03` `OTA_COMMIT` | — |
| → device | `0x04` `OTA_ABORT` | — |
| → device | `0x05` `OTA_REBOOT` | — |
| → device | `0x06` `OTA_STATUS` | — |
| ← device | `0x86` `OTA_STATUS_RESULT` | `result:1` `state:1` `bytes_received:u32le` `image_size:u32le` (10 bytes) |
| ← device | `0x8F` `OTA_RESULT` | `result:1` — reply for BEGIN/CHUNK/COMMIT/ABORT/REBOOT |

### 10.1 State machine

Four states (`mc_ota_state_t`), reported in `OTA_STATUS_RESULT`'s `state`
byte:

```
IDLE ──OTA_BEGIN(ok)──► RECEIVING ──OTA_COMMIT(ok)──► COMMITTED ──OTA_REBOOT(ok)──► (device reboots)
  ▲                         │                              │
  │                         │ any failure                  │ any failure
  └────── OTA_ABORT ────────┴──────────────► ERROR ◄────────┘
                                               │
                                          OTA_ABORT
                                               │
                                               ▼
                                             IDLE
```

| State | Wire value | Meaning |
|---|---|---|
| `IDLE` | 0 | No transfer in progress. `OTA_BEGIN` is the only op that does anything besides `OTA_STATUS`/`OTA_ABORT` (both harmless no-ops here). |
| `RECEIVING` | 1 | Signature already verified (at `OTA_BEGIN`); accepting `OTA_CHUNK`s in strict offset order. |
| `COMMITTED` | 2 | Image fully received, hash-verified, and flash-finalized into the inactive partition. Awaits an explicit `OTA_REBOOT` — nothing boots automatically. Safe to sit here indefinitely (e.g. rider finishes the ride first); a reconnect can resume by polling `OTA_STATUS` and going straight to `OTA_REBOOT`. |
| `ERROR` | 3 | Last attempt failed. Only `OTA_ABORT` clears it (returns to `IDLE`); no further chunks or commits are accepted until then. |

### 10.2 Trust model

An image must satisfy **both** of the following — either alone is not
sufficient:

1. It arrives over an already-authenticated BLE session (same
   challenge-response as every other channel, §6).
2. Its signature verifies against the project's release public key
   (`MC_OTA_RELEASE_PUBKEY`, `firmware/components/core/mc_ota_release_key.c`)
   — an Ed25519 signature over the image's SHA-512 digest, **not** the raw
   image bytes (this device has no PSRAM to buffer a multi-hundred-KB image
   before hashing it; verifying the signature over the 64-byte digest lets
   `OTA_BEGIN` reject a bad/unsigned image before a single chunk byte is
   accepted).

`OTA_BEGIN`'s `sha512`/`signature` fields and the actual transferred bytes
are cross-checked twice: once at `OTA_BEGIN` (signature over the declared
digest) and once at `OTA_COMMIT` (the running SHA-512 over every
`OTA_CHUNK` byte actually received must equal the declared digest — catches
transport corruption/truncation, which the signature check alone wouldn't).

See §10.4 for how a release image and its signature are produced.

### 10.3 Safe-state gating

`OTA_BEGIN` and `OTA_REBOOT` both additionally require
`!engine_running && !lv_cutoff_active` (the same `engine_running` voltage
detection as starter protection, §12.2, and the low-voltage cutoff, §12.2) —
otherwise they return `REJECTED`. Flashing while riding or while the battery
is critically low is refused. This is re-checked independently at
`OTA_REBOOT` time (not just inherited from `OTA_BEGIN`): a `COMMITTED` image
can sit safely in the inactive partition indefinitely, so nothing is lost if
the bike started moving between commit and reboot — the client just retries
`OTA_REBOOT` later. `OTA_CHUNK`/`OTA_COMMIT` do not re-check this — once
`OTA_BEGIN` has admitted the transfer, the currently-running image is
untouched throughout (writes land in the *inactive* partition), so there is
nothing unsafe about continuing to receive chunks.

### 10.4 Producing a signed release image

`tools/sign-firmware.py` turns a built `moto_ctrl_firmware.bin` into a
`.mcota` bundle — see `docs/PROTOCOL.md`'s companion note in that script's
`--help` and `tools/README.md` for the maintainer key-generation and signing
workflow. A `.mcota` file is a small header (magic, format version, the same
`image_size`/`sha512`/`signature` fields as `OTA_BEGIN`) followed by the raw
image bytes — the app parses that header and feeds its three fields plus
the image bytes straight into `OTA_BEGIN`/`OTA_CHUNK` verbatim. §10.5 below
covers how a `.mcota` bundle reaches the app in the first place.

`firmware/sim` never uses the real release key: it embeds its own fixed
TEST keypair (`firmware/sim/src/main.c`) so `ctest`/the itest suite/app tests
against the simulator can synthesize validly-signed test images without the
real release key ever being involved. `firmware/sim/itest/moto-client.mjs`'s
`otaTransfer()` is a worked reference implementation of the full begin/
chunk-loop/commit sequence against that test key.

### 10.5 App-side update delivery

The app has no way to reach any server except one narrow, permanent
exception to its offline-first design, made for this exact purpose: two
anonymous, read-only HTTPS GETs to one fixed, baked-in URL — a
version-manifest JSON file, then (only if the rider chooses to update) the
`.mcota` bundle it points to. Nothing else is ever sent or requested — no
identifiers, no accounts, and a failed/unreachable check must never block
BLE pairing or control of the board. See `CONTRIBUTING.md`'s "No cloud, no
telemetry, no accounts" section for the full constraints — this section
only documents the file shapes.

Manifest JSON:

```json
{
  "version": "1.2.0",
  "changelog": "Fixes X, adds Y.",
  "bundle_url": "https://.../moto-ctrl-1.2.0.mcota",
  "bundle_sha512": "<128 lowercase hex chars>",
  "bundle_size": 123456
}
```

- `version` is compared against the connected device's `STATUS` firmware
  version (§5) to decide whether to show "update available".
- `bundle_sha512`/`bundle_size` describe the `.mcota` **file** being
  downloaded (transport-integrity check only, done by the app right after
  download, before even attempting a BLE transfer) — distinct from the
  `sha512` field *inside* the `.mcota` header, which is the hash of the raw
  firmware image and is what the device's Ed25519 signature check (§10.2)
  actually relies on for security. A corrupt or tampered manifest can at
  worst point the app at a bad download, which the on-device signature
  check in §10.2 — the real security boundary — still catches.

`.mcota` bundle format (produced by `tools/sign-firmware.py`, parsed by the
app before driving `OTA_BEGIN`/`OTA_CHUNK`):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic: ASCII `"MCOT"` |
| 4 | 1 | format version (currently `1`) |
| 5 | 3 | reserved, must be 0 |
| 8 | 4 | `image_size` (u32le) — matches `OTA_BEGIN`'s field |
| 12 | 64 | `sha512` — SHA-512 of the raw image, matches `OTA_BEGIN`'s field |
| 76 | 64 | `signature` — Ed25519 signature over `sha512`, matches `OTA_BEGIN`'s field |
| 140 | `image_size` | the raw firmware image |

## 11. Lock / immobilizer channel (`2`, COMMAND) — authenticated

The immobilizer state machine's wire ops live on the **same COMMAND
channel** as `SET_OUTPUT` (§7) — they're commands like any other, just
routed to the lock engine instead of the output engine. Simple ops share
the existing `COMMAND_RESULT` `[req_opcode:u8][result:u8]` reply; the two
ops that return a payload get their own response opcode.

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x02` `LOCK` | — |
| → device | `0x03` `UNLOCK` | — |
| → device | `0x04` `LOCK_GET_CONFIG` | — |
| ← device | `0x84` `LOCK_CONFIG` | see below |
| → device | `0x05` `LOCK_SET_CONFIG` | see below |
| → device | `0x06` `CHEATCODE_SET` | `len:u8` `buttons:u8[len]` |
| → device | `0x07` `CHEATCODE_CLEAR` | — |
| → device | `0x08` `CHEATCODE_TEST` | `len:u8` `buttons:u8[len]` |
| ← device | `0x88` `CHEATCODE_TEST_RESULT` | `result:u8` `match:u8` |
| → device | `0x09` `TRANSFER_OWNERSHIP` | — |

### 11.1 State machine

Four states, reported as the status wire's `lock_state` byte (§5):

| State | Meaning | Wire value |
|---|---|---|
| `DISABLED` | No immobilizer configured (no cheat-code set, or the immobilizer toggle is off). Nothing inhibited. | `UNLOCKED` (3) |
| `UNLOCKED` | Immobilizer enabled and authorized. Ignition permitted. | `UNLOCKED` (3) |
| `PARKED` | Enabled and unlocked, engine off *and* ignition output off. Auto-lock grace timer running. | `PARKED` (1) |
| `LOCKED` | Enabled and immobilized. Ignition and starter outputs refused, from any source. | `LOCKED` (2) |

```
                 immobilizer enabled (cheat-code set)
   ┌──────────┐  ───────────────────────────────────►  ┌──────────┐
   │ DISABLED │                                          │ UNLOCKED │◄────────┐
   └──────────┘  ◄───────────────────────────────────   └────┬─────┘         │
        ▲          disable / factory reset / transfer         │ engine off &  │ engine on OR
        │                                                      │ ign not live │ ignition live
        │  factory reset / ownership transfer                 ▼               │
        │  (from any state)                              ┌──────────┐         │
        └────────────────────────────────────────────    │  PARKED  │─────────┘
                                                           └────┬─────┘
                          grace timer elapsed  OR              │  guard: !engine_running
                          explicit LOCK (authed)                ▼  && !ignition_live
                                                           ┌──────────┐
                                 any enabled unlock        │  LOCKED  │
                                 method succeeds  ◄─────── │ (+ cheat-│
                                 → UNLOCKED                │  code    │
                                                            │ backoff) │
                                                            └──────────┘
```

Boot restore is ride-safe (see `CONTRIBUTING.md`'s safety requirement #1):
the device never restores into `LOCKED` while the engine appears to be
running or the ignition output is already live — a brownout mid-ride
always restores the bike running, never immobilized. `LOCK` (explicit)
refuses with `REJECTED` unless `!engine_running && !ignition_live` holds
at the moment of the request, mirroring safety requirement #2 ("lock state
may only be entered from parked, never while ignition output is live")
exactly.

### 11.2 Method truth table

Three unlock methods, controlled by `LOCK_SET_CONFIG`'s `methods_mask` bits
(`PHONE = 0x01`, `IGNITION_SWITCH = 0x02`) plus the always-on cheat-code:

- **The button cheat-code is not a `methods_mask` bit** — it is always
  active whenever the immobilizer is enabled, and cannot be disabled while
  it is (`CONTRIBUTING.md` safety requirement #3's mandatory fallback).
  `LOCK_SET_CONFIG` with `immobilizer_enabled=1` is refused (`REJECTED`)
  unless a cheat-code is already set via `CHEATCODE_SET`.
- **Every enabled method works simultaneously** — there is no "pick one."
- **Phone-as-key unlock is edge-triggered**, not proximity-polled: it fires
  when a session's challenge-response *newly* succeeds (§6), not
  continuously while a session stays connected. This is deliberate — a
  level-triggered design would make an explicit `LOCK` self-defeating
  while the same phone that just issued it stays connected (it would
  immediately re-unlock). A phone that was already authenticated before
  the bike locked stays able to unlock via the explicit `UNLOCK` op.
- **Ignition-switch mode does not replace the cheat-code.** The handlebar
  buttons have no wiring of their own to fail; a dedicated ignition-switch
  wire is a single point of failure that must never be the *only*
  fallback. Both may be enabled together.
- **An in-range/authenticated phone never suppresses the cheat-code** —
  physical entry works regardless of phone presence (for a dead phone, or
  lending the bike to someone without pairing their phone).
- A fresh or factory-reset device boots with the immobilizer **disabled**
  until the owner configures a method — a device can never lock with no
  configured way back in.

### 11.3 `LOCK` / `UNLOCK`

`LOCK`: locks now if the immobilizer is enabled and the guard
(`!engine_running && !ignition_live`) holds. `REJECTED` if the guard fails
or the immobilizer isn't enabled. Idempotent — locking an already-`LOCKED`
bike returns `OK`. Works from `UNLOCKED` or `PARKED` (doesn't require
waiting for the auto-lock grace timer).

`UNLOCK`: phone-as-key unlock from an authenticated session (the PHONE
`methods_mask` bit must be set, else `REJECTED`). A no-op `OK` if the bike
isn't currently `LOCKED`. This is the explicit counterpart to the
edge-triggered auto-unlock described in §11.2 — use it when the session
authenticated *before* the bike became `LOCKED`.

### 11.4 Cheat-code: storage, entry, and lockout

The cheat-code is **never transmitted or stored in cleartext** — the
device stores a salted SHA-512 hash (`salt(16) || len(1) || buttons`) in
its own NVS blob, deliberately **not** part of the exportable JSON config
(§9) — same doctrine as the keystore (§6): a flash/NVS dump or a restored
config backup cannot reveal or replay it.

- `CHEATCODE_SET` `[len:u8][buttons:u8[len]]`: `len` must be 4–10, each
  button index 0–7 (matching the combo matcher's own bounds). Replaces any
  existing code; resets the in-progress entry buffer and lockout counter.
  `BAD_REQUEST` if `len`/buttons are out of range.
- `CHEATCODE_CLEAR`: `REJECTED` while `immobilizer_enabled` is true (the
  mandatory fallback can't be removed out from under an active
  immobilizer — disable the immobilizer first).
- `CHEATCODE_TEST` `[len:u8][buttons:u8[len]]` → `CHEATCODE_TEST_RESULT`
  `[result:u8][match:u8]`: a **practice mode** — pure hash comparison, no
  side effects (never touches the entry buffer, the lockout counter, or
  lock state). Authenticated-only, same trust boundary as every other
  command on this channel.
- **Physical entry** is not an RPC — there is no "submit code" opcode. The
  device buffers short button presses (from the handlebar input engine)
  while the bike is `LOCKED`, evaluating as soon as the buffer reaches the
  configured length, within a configurable entry window
  (`cheatcode_window_ms`, default 5000ms; an incomplete/timed-out entry is
  not counted as wrong). Presses are ignored entirely while not `LOCKED`,
  so ordinary riding (turn-signal/horn presses on the same 8 buttons)
  never accumulates wrong-entry counts.

**Wrong-entry backoff** (byte 15 bit 0 of the status wire reports whether
it's currently active): the first **5** wrong entries are free (no
cooldown — cold hands, a fumbled sequence, roadside stress). The 6th wrong
entry starts a progressive cooldown: 15s, then 30s, then a 60s cap. During
backoff, cheat-code presses are ignored entirely — **phone-as-key and the
ignition switch are never gated by this backoff**, so the rider is never
actually locked out (`CONTRIBUTING.md` safety requirement #3). The counter
resets on any successful
unlock (any method) or after 5 minutes with no attempts. This state is
RAM-only — it resets on reboot, since the cheat-code is a low-entropy
convenience fallback, not the primary security boundary.

### 11.5 `LOCK_GET_CONFIG` / `LOCK_SET_CONFIG`

`LOCK_GET_CONFIG` (no payload) → `LOCK_CONFIG`:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | `immobilizer_enabled` |
| 1 | 1 | `methods_mask` (bit 0 = PHONE, bit 1 = IGNITION_SWITCH) |
| 2 | 1 | `ignition_switch_input` (0–7, or `0xFF` = none) |
| 3 | 2 | `auto_lock_grace_ms` (u16le) |
| 5 | 2 | `cheatcode_window_ms` (u16le) |
| 7 | 1 | `cheatcode_set` |
| 8 | 1 | `cheatcode_len` |

Never includes the cheat-code salt/hash — the device is the only party
that ever needs them.

`LOCK_SET_CONFIG` `[immobilizer_enabled:u8][methods_mask:u8][ignition_switch_input:u8][auto_lock_grace_ms:u16le][cheatcode_window_ms:u16le]`
→ `COMMAND_RESULT`. Validated before applying (`REJECTED` on failure, config
unchanged): enabling requires a cheat-code already set (§11.2) *and* an
`ignition`-function output channel configured (§9 — nothing to immobilize
otherwise); enabling `IGNITION_SWITCH` requires a valid `ignition_switch_input`
(0–7). Leaves the cheat-code itself untouched — it's only ever changed via
`CHEATCODE_SET`/`CHEATCODE_CLEAR`. `ignition_switch_input` reuses an input
index exactly like `starter_interlock_input` (§9) — it is not a separate
physical signal, just a debounced-level read of one of the 8 handlebar
inputs (a maintained switch wired to one of CN2's button inputs, not a
momentary button).

### 11.6 Ownership transfer & factory reset

`TRANSFER_OWNERSHIP` (authenticated, no payload): wipes every enrolled key
(§6) and resets the lock config to factory defaults (immobilizer disabled,
no cheat-code, no ignition-switch assignment) in one atomic operation,
releasing any active immobilize. Requires an authenticated session, so a
thief can't wipe the owner's keys. After it: the keystore is empty, so
trust-on-first-use enrollment (§6) reopens for the next owner, and the
immobilizer stays disabled until they configure a new cheat-code.

The **physical** factory reset (`CONTRIBUTING.md` safety requirement #3:
hold BOOT for 10s at power-on, confirmed by a distinct pattern before
wiping) reaches the same
end state without any session or key — it's for the no-phone/lost-owner
case. It is bench/board-only; there is no BLE/WebSocket op for it, since
by definition it works with no live session. See
`firmware/main/factory_reset.c`.

## 12. Diagnostics channel (`2`, COMMAND) — authenticated

Current-sense diagnostics, battery-derived `engine_running` (see
`CONTRIBUTING.md` safety requirement #6) and low-voltage cutoff (safety
requirement #7) live on the same COMMAND channel as `SET_OUTPUT` (§7) and
the lock ops (§11). Same conventions as
§11: `COMMAND_RESULT` for simple ops, a dedicated response opcode where
there's a payload — but every dedicated response here **leads with a
`result:u8` byte** (unlike `LOCK_CONFIG`), so a device with diagnostics
unavailable can still reply with a well-formed, zeroed frame carrying
`REJECTED` rather than needing a second error path.

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x0A` `DIAG_GET` | — |
| ← device | `0x8A` `DIAG_RESULT` | see §12.5 |
| → device | `0x0B` `DIAG_GET_CONFIG` | — |
| ← device | `0x8B` `DIAG_CONFIG` | see §12.6 |
| → device | `0x0C` `DIAG_SET_CONFIG` | see §12.6 |
| → device | `0x0D` `DIAG_GET_CALIB` | — |
| ← device | `0x8D` `DIAG_CALIB` | see §12.7 |
| → device | `0x0E` `DIAG_SET_CALIB` | see §12.7 |
| → device | `0x0F` `DIAG_LEARN` | `channel:u8` (`0xFF` = every energized channel) |

### 12.1 Current-sense sampling and fault classification

Hardware (`hardware/PINOUT.md`): 6 PROFETs (2 output channels each) share
one current-sense ADC line, muxed by a shared `DSEL` select plus one
device's `DEN` at a time — only one channel's current can be read at once,
so the device **round-robin samples one actually-energized channel per
~10ms tick** rather than all 12 simultaneously. A channel that is not
actually energized — commanded off, *or* suppressed by the low-voltage
cutoff (§12.4) — always reports `0mA` and is never fault-classified; a
channel genuinely drawing no current only faults if it is supposed to be
driving a load right now.

Per channel, the device classifies the calibrated reading against two
configurable thresholds (§12.6, §9): below `open_load_ma` while energized ⇒
`OPEN_LOAD` (1); above `overcurrent_ma` ⇒ `OVERCURRENT` (2); otherwise
`NONE` (0). These three values are `mc_diag_fault_t`; a value of `3`
(`SHORT`) exists only in the pre-hardware simulator's fault-injection wire
shape (`firmware/sim/src/sim_protocol.h`) and is never produced by real
classification.

### 12.2 Battery, `engine_running`, and the low-voltage cutoff

The battery-sense ADC line is read every tick and calibrated the same way
(§12.6). Two signals are derived from it, each with its own configurable
threshold **and hysteresis band** (§9) to avoid chatter at the boundary:

- **`engine_running`** (see `CONTRIBUTING.md` safety requirement #6): set
  once the battery reading is at or above `engine_run_mv` (default 13.8V —
  deliberately well above a fully-charged LiFePO4 pack's resting voltage,
  so a healthy-but-idle battery is never mistaken for a running/charging
  engine), cleared once it drops below `engine_run_mv -
  engine_run_hysteresis_mv`. This drives the starter-protection guard
  (§7's `SET_OUTPUT` rejection reasons) and the lock state machine's
  parked-detection guard (§11.1).
- **Low-voltage cutoff** (safety requirement #7): engages once the battery
  reading drops below `lv_cutoff_mv` (default 11.8V for LiFePO4) **and only
  while `!engine_running`** — a charging system holding voltage up must never be
  second-guessed by a stale low reading. Recovers once the reading is back
  at or above `lv_cutoff_mv + lv_cutoff_hysteresis_mv`. A raw reading of
  exactly `0mV` (an unread/failed ADC line, far more likely than a
  genuinely dead battery) is treated as "unknown" and never engages the
  cutoff on its own.

**Cutoff behavior is a suppression, not a command change.** While active,
every **non-essential** output channel — everything except `ignition`,
`brake`, `headlight_hi`, and `headlight_lo` (`CONTRIBUTING.md` safety
requirement #1's "never drop these mid-ride" set; the starter is excluded
too, since it already has its own dedicated guards) — is driven off at the
hardware regardless of its
commanded state, but `commanded_on` itself, and therefore
**`output_state_mask` (§5), is left untouched**. This mirrors a real
vehicle: a blown fuse or protection circuit doesn't move the switch. The
app must read status wire byte 15 bit 1 (`lvCutoffActive`), not a change in
`output_state_mask`, to know a channel is currently suppressed; recovery
needs no separate command — every channel returns to its last-commanded
state automatically.

### 12.3 `DIAG_GET` / `DIAG_RESULT`

`DIAG_GET` (no payload) → `DIAG_RESULT`:

```
[result:u8]
[current_ma:u16le] × 12   (channels 0-11, in order)
[fault:u8] × 12           (mc_diag_fault_t, channels 0-11, in order)
```

37 bytes total (plus the leading opcode byte on the wire) — the live
per-channel readings from §12.1, as of each channel's last round-robin
sample.

### 12.4 `DIAG_GET_CONFIG` / `DIAG_SET_CONFIG`

`DIAG_GET_CONFIG` (no payload) → `DIAG_CONFIG`:

```
[result:u8]
([open_load_ma:u16le][overcurrent_ma:u16le]) × 12   (channels 0-11)
[lv_cutoff_mv:u16le][lv_cutoff_hysteresis_mv:u16le]
[engine_run_mv:u16le][engine_run_hysteresis_mv:u16le]
```

57 bytes total. `DIAG_SET_CONFIG` sends the same shape **without** the
leading `result` byte (56 bytes) → `COMMAND_RESULT`. `REJECTED` if any
channel's `open_load_ma >= overcurrent_ma` (never classifiable as
`OVERCURRENT` — not unsafe, just certainly a misconfiguration); config
unchanged on rejection. On success, both the live engine and the persisted
`mc_config_t.diagnostics` (§9) are updated — a config export/import
round-trips these thresholds too.

### 12.5 `DIAG_LEARN`

`DIAG_LEARN` `[channel:u8]` (`0xFF` = every currently-energized channel) →
`COMMAND_RESULT`. Samples the live current of each targeted channel and
sets its `open_load_ma` to roughly half the measured healthy draw — a
subsequent real fault (bulb blows, connector opens) still trips well above
zero but comfortably below a healthy reading (thresholds are always
learnable, never hardcoded to an assumed bulb). Never touches
`overcurrent_ma`. `REJECTED` if the targeted channel (or, for `0xFF`, every
channel) isn't actually energized — there's nothing to measure.

### 12.6 `DIAG_GET_CALIB` / `DIAG_SET_CALIB`

Board-specific analog calibration — deliberately **not** part of the config
JSON (§9) or a config export/import: applying board A's gain/offset/kILIS
constants to board B would silently misreport board B's real current and
voltage. Persisted in its own NVS blob and, unlike a factory reset /
`TRANSFER_OWNERSHIP` (§11.6), **not wiped by either** — it describes the
physical board, not the outgoing owner.

`DIAG_GET_CALIB` (no payload) → `DIAG_CALIB`:

```
[result:u8]
[is_gain:f32le][is_offset_mv:i16le]
[kilis:f32le]
[vbat_gain:f32le][vbat_offset_mv:i16le]
```

17 bytes total. `DIAG_SET_CALIB` sends the same shape without the leading
`result` byte (16 bytes) → `COMMAND_RESULT`. `f32le` is IEEE754 binary32,
little-endian — standard on every platform this project targets (ESP32-S3,
x86/ARM hosts, and JS via `DataView.getFloat32(offset, true)`).
`is_gain`/`is_offset_mv` correct the current-sense (`PROFET_IS`) reading;
`kilis` is the BTS7008-2EPA datasheet's current-sense ratio
(`I[mA] = (V_IS[mV] / 2000) * kilis`, 2kΩ sense resistor, `hardware/
PINOUT.md`); `vbat_gain`/`vbat_offset_mv` correct the battery-divider
reading. No validation beyond payload shape — these are installer/bench
values, trusted like the rest of this authenticated channel.

## 13. Flashers / PWM channel (`2`, COMMAND) — authenticated

Turn signals, hazard, brake flasher, and PWM dimming. Mode, duty, timing,
and the brake-switch input assignment all ride the config JSON (§9) — there
are no dedicated get/config wire ops here, unlike lock (§11) or diagnostics
(§12), since output config never had its own dedicated channel to begin
with (function/name/mode have always been generic config JSON fields). The
**one** dedicated opcode here is `HAZARD_PRESS`.

### 13.1 Turn signals — `SET_OUTPUT` (§7), no new opcode

A plain turn-signal press is just `SET_OUTPUT` on the `turn_l`/`turn_r`-
function channel — the policy below is embedded in the device's output
engine itself (`mc_output_set()`), so it applies automatically to any
caller (a handlebar button locally, or this wire op), the same way starter
protection and the low-voltage cutoff already do:

- Turning a `turn_l`/`turn_r` channel **on** forces the opposite side off
  (mutual exclusion — only one side signals at a time) and arms the
  configured auto-cancel timer (`outputs.turn_auto_cancel_ms`, §9; `0`
  disables it — manual toggle only).
- Turning it **off** clears that channel's pending auto-cancel timer.
- The auto-cancel timer expiring is **not** a wire event — a client that
  cares should just poll `STATUS` (§5); `output_state_mask` clears the bit
  the same way any other commanded-off transition does.

A `flash_turn`-mode channel (§9's `mode`) blinks at
`outputs.turn_flash_period_ms` while commanded on; the blink itself is not
observable on the wire (see §13.3).

### 13.2 Hazard — `HAZARD_PRESS`

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x10` `HAZARD_PRESS` | — |
| ← device | `0x81` `COMMAND_RESULT` | `req_opcode:u8` `result:u8` |

Toggles both `turn_l` and `turn_r` channels together: if either is
currently off, turns both on; if both are already on, turns both off. This
needs its own opcode because "both sides on together" can't be expressed
as two `SET_OUTPUT` calls without the mutual-exclusion rule above
cancelling the first one. No auto-cancel timer is armed either way —
hazards stay on until pressed again (like a real hazard switch), and
remain subject to the low-voltage cutoff (§12) like any non-essential
channel. `REJECTED` if neither a `turn_l` nor a `turn_r` channel is
configured.

### 13.3 Brake flasher

A `brake`-function channel defaults to plain `on` mode (steady, follows
commanded state) — the attention-pulse burst is opt-in via
`mode: "flash_brake"`, off by default, because flash patterns are not
legal in every jurisdiction (see `CONTRIBUTING.md` safety requirement #5).
In `flash_brake` mode, the channel's off→on transition plays a burst of
`outputs.brake_flash_pulse_count` pulses (`_on_ms` lit, `_off_ms` dark
each) before settling solid; the off-phases are deliberately short
relative to the on-phases so the light spends the overwhelming majority of
the burst lit, in keeping with that same requirement that the brake light
is on whenever the brake input is asserted. Turning the channel off is
always immediate — never mid-pattern.

There's no dedicated "brake pressed" opcode: a physical brake-lever/pedal
switch is assigned to one of the 8 button inputs via
`outputs.brake_switch_input` (§9) and read as a maintained level (mirrors
`starter_interlock_input`'s pattern), which the firmware turns into an
ordinary `SET_OUTPUT`-equivalent transition on the `brake`-function
channel — the same path an app-issued `SET_OUTPUT` on that channel would
take (useful for bench testing without a wired switch).

### 13.4 PWM dimming

`mode: "pwm"` (§9) drives a channel at a steady `pwm_duty_pct` (1–100)
while commanded on, instead of full on/off. Opt-in per channel, off by
default — driver-based LED lamps can misbehave under PWM. Flasher
patterns (§13.1, §13.3) never use PWM —
they're always full on/off switching, so every lamp type works without
hyperflash workarounds.

### 13.5 What's not on the wire

No status-snapshot changes this phase: a client can already tell whether a
turn/hazard signal is commanded active from `output_state_mask` (§5) on
the `turn_l`/`turn_r` channels, the same way it already reads any other
channel's intent. Instantaneous blink phase / burst-pulse phase is **not**
exposed over the wire — same design choice as the low-voltage cutoff
(§12): the app shows commanded intent, not sub-second HAL state, and a
phone polling at a human-relevant rate has no real use for it anyway.

## 14. Security summary

- Link is encrypted and bonded via LE Secure Connections.
- Authority comes from the Ed25519 challenge-response, not the link or MAC.
- The device stores **only public keys**, and only a **salted hash** of the
  cheat-code; a flash/NVS dump cannot unlock the bike, forge a key, or
  recover the cheat-code.
- Nonces are single-use and per-session; signatures cannot be replayed.
- The starter is never actuable over this protocol; neither is the
  ignition output while the immobilizer is `LOCKED` (§11).
- Authentication is one-way (client proves possession of an enrolled key to
  the device). A future revision may add device-to-client authentication.

## 15. Event log (COMMAND channel `2`) — authenticated

A persisted, fixed-size ring buffer (`mc_event_log.c`,
`MC_EVENT_LOG_SLOT_COUNT` = 1024 records) of security/safety-relevant
events: lock state transitions, key enroll/revoke/ownership-transfer,
factory reset, cheat-code lockout, OTA begin/success/failure, low-voltage
cutoff enter/exit. Deliberately narrow scope — routine output toggles and
diagnostics faults are already visible live via `STATUS`/`DIAG` (§5, §12)
and would just be noise here.

Rides the same COMMAND channel as `SET_OUTPUT`/lock/diagnostics ops (§7,
§11, §12) and the same authenticated-session gate.

| Direction | Opcode | Payload |
|---|---|---|
| → device | `0x11` `EVENT_LOG_GET` | `since_seq:u32le` (0 = oldest available) |
| ← device | `0x91` `EVENT_LOG_CHUNK` | one or more frames, see below |

`EVENT_LOG_GET` requests every record with `seq > since_seq`. The reply is
one or more `EVENT_LOG_CHUNK` frames (mirrors `CONFIG_CHUNK`'s chunked-
reassembly idiom, keyed by record index instead of byte offset, capped at
`MC_PROTOCOL_EVENT_LOG_CHUNK_RECORDS` = 10 records per frame to keep the
device-side stack buffer small):

```
index:u16le  total:u16le  count:1  then `count` × 12-byte records, oldest-first
```

Each 12-byte record (`mc_event_record_t`):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `seq` — 1-based, monotonic, never reused |
| 4 | 4 | `uptime_ms` — device uptime at the event (no RTC on this hardware) |
| 8 | 1 | `type` — `mc_event_type_t`, see table below |
| 9 | 1 | `arg0` — meaning depends on `type` |
| 10 | 1 | `arg1` — reserved, always 0 today |
| 11 | 1 | reserved, must be 0 |

An empty log (or a device with no event log attached) is a single frame
with `index=0, total=0, count=0` — the same empty-result shape
`CONFIG_CHUNK` uses. If the log has been trimmed (ring buffer eviction) or
reset (ownership transfer/factory reset) since `since_seq`, records below
the retained range are simply not returned — there is no separate
"gap detected" signal; a client that wants everything should track the
highest `seq` it has seen and pass 0 after a reconnect if it suspects a gap,
rather than assuming continuity.

| `type` | Value | `arg0` | Meaning |
|---|---|---|---|
| `LOCK_ENGAGED` | 1 | unused | Immobilizer entered `LOCKED` (§11.1) |
| `LOCK_RELEASED` | 2 | unlock method (0=phone-auto, 1=explicit, 2=cheatcode, 3=ignition-switch, 4=transfer/reset) | Immobilizer left `LOCKED` |
| `KEY_ENROLLED` | 3 | keystore slot | A phone key was enrolled (§6) |
| `KEY_REVOKED` | 4 | keystore slot | A phone key was revoked |
| `OWNERSHIP_TRANSFERRED` | 5 | unused | `TRANSFER_OWNERSHIP` completed (§11.6) |
| `FACTORY_RESET` | 6 | 0 = physical BOOT-hold | Factory reset performed |
| `CHEATCODE_LOCKOUT` | 7 | consecutive-wrong count at trip (capped at 255) | Cheat-code entry backoff engaged (§11.4) |
| `OTA_BEGIN` | 8 | unused | `OTA_BEGIN` accepted (§10) |
| `OTA_SUCCESS` | 9 | unused | `OTA_COMMIT` succeeded |
| `OTA_FAILURE` | 10 | `mc_ota_result_t` of the failure | An OTA step failed (only the security-relevant `BAD_SIGNATURE` rejection at `OTA_BEGIN`, plus any `OTA_COMMIT` failure, are logged — routine unsafe-state rejections on retry aren't) |
| `LV_CUTOFF_ENTER` | 11 | unused | Low-voltage cutoff engaged (§12.2) |
| `LV_CUTOFF_EXIT` | 12 | unused | Low-voltage cutoff cleared |
