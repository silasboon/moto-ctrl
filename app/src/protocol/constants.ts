/**
 * Wire constants for the real MOTO-CTRL protocol, mirroring
 * firmware/components/core/include/mc_protocol.h and docs/PROTOCOL.md.
 *
 * Deliberately does NOT include anything from firmware/sim/src/sim_protocol.h
 * (SIM_CH_DEBUG and friends) — that channel exists only on the simulator's
 * debug GUI and must never be reachable from this app, which talks to real
 * hardware. Keep this file in sync with mc_protocol.h by hand, the same way
 * firmware/sim/gui/app.js and firmware/sim/itest/moto-client.mjs already do
 * independently — see docs/TESTING.md.
 */

export const MC_CH = {
  STATUS: 0,
  AUTH: 1,
  COMMAND: 2,
  CONFIG: 3,
  OTA: 4,
} as const;

export const MC_OP = {
  STATUS_GET: 0x01,
  STATUS: 0x81,

  AUTH_BEGIN: 0x01,
  AUTH_CHALLENGE: 0x81,
  AUTH_RESPONSE: 0x02,
  AUTH_RESULT: 0x82,
  ENROLL: 0x10,
  ENROLL_RESULT: 0x90,
  KEY_LIST: 0x11,
  KEY_LIST_RESULT: 0x91,
  KEY_REVOKE: 0x12,
  KEY_REVOKE_RESULT: 0x92,

  SET_OUTPUT: 0x01,
  COMMAND_RESULT: 0x81,

  CONFIG_READ: 0x01,
  CONFIG_CHUNK: 0x81,
  CONFIG_WRITE_BEGIN: 0x02,
  CONFIG_WRITE_CHUNK: 0x03,
  CONFIG_WRITE_COMMIT: 0x04,
  CONFIG_WRITE_RESULT: 0x82,

  // OTA, all on the OTA channel (docs/PROTOCOL.md §10).
  OTA_BEGIN: 0x01,
  OTA_CHUNK: 0x02,
  OTA_COMMIT: 0x03,
  OTA_ABORT: 0x04,
  OTA_REBOOT: 0x05,
  OTA_STATUS: 0x06,
  OTA_STATUS_RESULT: 0x86,
  OTA_RESULT: 0x8f,

  // Event log, on the COMMAND channel (docs/PROTOCOL.md §15).
  EVENT_LOG_GET: 0x11,
  /* Button-identification learn mode (docs/PROTOCOL.md §14.1). INPUT_EVENT
   * is unsolicited: it can arrive between any other COMMAND request and its
   * reply, so a client must not treat an unexpected frame as a protocol
   * error. */
  INPUT_LEARN: 0x12,
  INPUT_EVENT: 0x92,
  EVENT_LOG_CHUNK: 0x91,

  // Lock/immobilizer, all on the COMMAND channel (docs/PROTOCOL.md §11).
  LOCK: 0x02,
  UNLOCK: 0x03,
  LOCK_GET_CONFIG: 0x04,
  LOCK_CONFIG: 0x84,
  LOCK_SET_CONFIG: 0x05,
  CHEATCODE_SET: 0x06,
  CHEATCODE_CLEAR: 0x07,
  CHEATCODE_TEST: 0x08,
  CHEATCODE_TEST_RESULT: 0x88,
  TRANSFER_OWNERSHIP: 0x09,

  // Diagnostics, all on the COMMAND channel (docs/PROTOCOL.md §12).
  DIAG_GET: 0x0a,
  DIAG_RESULT: 0x8a,
  DIAG_GET_CONFIG: 0x0b,
  DIAG_CONFIG: 0x8b,
  DIAG_SET_CONFIG: 0x0c,
  DIAG_GET_CALIB: 0x0d,
  DIAG_CALIB: 0x8d,
  DIAG_SET_CALIB: 0x0e,
  DIAG_LEARN: 0x0f,

  // Flashers/PWM (docs/PROTOCOL.md §13). Plain turn-signal control
  // still just uses SET_OUTPUT above — mutual exclusion + auto-cancel are
  // embedded device-side in mc_output_set() itself — this is the only new
  // opcode. Mode/duty/timing/brake-switch-input all ride the CONFIG
  // channel's existing JSON, same as starterInterlockInput always has.
  HAZARD_PRESS: 0x10,
} as const;

/** mc_diag_fault_t (docs/PROTOCOL.md §12). */
export const DIAG_FAULT = {
  NONE: 0,
  OPEN_LOAD: 1,
  OVERCURRENT: 2,
} as const;

/** Sentinel channel value for MC_OP_DIAG_LEARN meaning "every currently
 * energized channel", not a single index. */
export const DIAG_LEARN_ALL = 0xff;

/** mc_ota_state_t (docs/PROTOCOL.md §10.1), OTA_STATUS_RESULT's state byte. */
export const OTA_STATE = {
  IDLE: 0,
  RECEIVING: 1,
  COMMITTED: 2,
  ERROR: 3,
} as const;

/** mc_event_type_t (docs/PROTOCOL.md §15), each EVENT_LOG_CHUNK record's type byte. */
export const EVENT_TYPE = {
  LOCK_ENGAGED: 1,
  LOCK_RELEASED: 2,
  KEY_ENROLLED: 3,
  KEY_REVOKED: 4,
  OWNERSHIP_TRANSFERRED: 5,
  FACTORY_RESET: 6,
  CHEATCODE_LOCKOUT: 7,
  OTA_BEGIN: 8,
  OTA_SUCCESS: 9,
  OTA_FAILURE: 10,
  LV_CUTOFF_ENTER: 11,
  LV_CUTOFF_EXIT: 12,
} as const;

const EVENT_TYPE_NAMES: Record<number, string> = Object.fromEntries(
  Object.entries(EVENT_TYPE).map(([name, value]) => [value, name]),
);

export function eventTypeName(type: number): string {
  return EVENT_TYPE_NAMES[type] ?? `UNKNOWN(${type})`;
}

/** arg0 values for MC_EVT_LOCK_RELEASED records (docs/PROTOCOL.md §15). */
export const EVENT_UNLOCK_METHOD_NAMES: Record<number, string> = {
  0: 'phone (auto)',
  1: 'phone (explicit)',
  2: 'cheat-code',
  3: 'ignition switch',
  4: 'ownership transfer / factory reset',
};

/** Bytes per OTA_CHUNK write. BLE 5's practical ATT payload ceiling is
 * 512 bytes (docs/PROTOCOL.md §2) — chosen to fit comfortably under that
 * once the 5-byte [opcode][offset:u32le] header is added. */
export const OTA_CHUNK_BYTES = 500;

/** MC_OTA_MAX_IMAGE_SIZE (mc_protocol.h) — must track partitions.csv's
 * ota_0/ota_1 partition size. */
export const OTA_MAX_IMAGE_SIZE = 0x180000;

/** `.mcota` bundle format (docs/PROTOCOL.md §10.5, tools/sign-firmware.py):
 * a fixed 140-byte header followed by the raw image. */
export const MCOTA_MAGIC = 'MCOT';
export const MCOTA_HEADER_BYTES = 140;

/** AGENTS.md's "Exception: firmware update check/download" — the one
 * baked-in URL the app is permitted to reach outside of BLE, and the only
 * network calls it ever makes. Point this at wherever release manifests
 * are published (e.g. a GitHub Releases asset) before shipping; until
 * then it's a placeholder, same doctrine as mc_ota_release_key.c's
 * placeholder public key on the firmware side — a value with no real
 * release behind it yet is safer than guessing at one. */
export const UPDATE_MANIFEST_URL =
  'https://github.com/silasboon/moto-ctrl/releases/latest/download/manifest.json';

/** MC_LOCK_METHOD_* wire bits for lock config's methods_mask (mc_protocol.h).
 * The button cheat-code is not a bit here — it's always active whenever
 * immobilizerEnabled is set (AGENTS.md #3: mandatory fallback). */
export const LOCK_METHOD = {
  PHONE: 1 << 0,
  IGNITION_SWITCH: 1 << 1,
} as const;

export const MC_RESULT = {
  OK: 0,
  UNAUTHENTICATED: 1,
  BAD_REQUEST: 2,
  REJECTED: 3,
  ENROLL_DENIED: 4,
  KEYSTORE_FULL: 5,
  NOT_FOUND: 6,
  NOT_IMPLEMENTED: 7,
  INTERNAL: 8,
} as const;

export type McResult = (typeof MC_RESULT)[keyof typeof MC_RESULT];

export function resultName(r: number): string {
  const entry = Object.entries(MC_RESULT).find(([, v]) => v === r);
  return entry ? entry[0] : `UNKNOWN(${r})`;
}

export const MC_LOCK_STATE = {
  0: 'UNKNOWN',
  1: 'PARKED',
  2: 'LOCKED',
  3: 'UNLOCKED',
} as const;

/** Numeric counterpart of MC_LOCK_STATE, for comparisons (e.g.
 * `status.lockState === LOCK_STATE.LOCKED`). mc_lock_state_t, mc_status.h. */
export const LOCK_STATE = {
  UNKNOWN: 0,
  PARKED: 1,
  LOCKED: 2,
  UNLOCKED: 3,
} as const;

export const AUTH_CONTEXT = 'moto-ctrl-auth-v1';
export const AUTH_CONTEXT_LEN = 17;
/* Must track MC_CONFIG_JSON_MAX in
 * firmware/components/core/include/mc_config_json.h — that header carries
 * the measured size accounting. If this is smaller, the app refuses config
 * writes the device would have accepted. */
export const CONFIG_JSON_MAX = 6144;
export const CONFIG_CHUNK_BYTES = 128;
export const OUTPUT_COUNT = 12;
export const INPUT_COUNT = 8;
export const STATUS_WIRE_LEN = 16;
export const MAX_ENROLLED_KEYS = 8;

/** GATT UUID for a channel byte, per docs/PROTOCOL.md §2: base
 * `5a4f00XX-9b1e-4f8a-9c2d-1a2b3c4d5e6f`, only the XX byte varies. Used for
 * both the service and characteristic UUID of a channel (they share the
 * same XX in this scheme; see ble_uuids.h). */
export function channelUuid(byte: number): string {
  const hex = byte.toString(16).padStart(2, '0');
  return `5a4f00${hex}-9b1e-4f8a-9c2d-1a2b3c4d5e6f`;
}

export interface ChannelGatt {
  service: string;
  characteristic: string;
}

/** Service/characteristic UUID pairs per channel, from docs/PROTOCOL.md §2's
 * table (service XX, characteristic XX). */
export const CHANNEL_GATT: Record<number, ChannelGatt> = {
  [MC_CH.STATUS]: {
    service: channelUuid(0x10),
    characteristic: channelUuid(0x11),
  },
  [MC_CH.AUTH]: {
    service: channelUuid(0x20),
    characteristic: channelUuid(0x21),
  },
  [MC_CH.COMMAND]: {
    service: channelUuid(0x20),
    characteristic: channelUuid(0x22),
  },
  [MC_CH.CONFIG]: {
    service: channelUuid(0x30),
    characteristic: channelUuid(0x31),
  },
  [MC_CH.OTA]: {
    service: channelUuid(0x40),
    characteristic: channelUuid(0x41),
  },
};

export const DEVICE_NAME = 'MOTO-CTRL';
