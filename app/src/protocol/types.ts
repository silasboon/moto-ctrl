/**
 * Data shapes for the real protocol, mirroring docs/PROTOCOL.md §5 (status)
 * and §9 (config JSON) / mc_config_json.c exactly. Keep in sync by hand.
 */

import { OUTPUT_COUNT } from './constants';

export interface Status {
  fwMajor: number;
  fwMinor: number;
  fwPatch: number;
  lockState: number;
  uptimeMs: number;
  batteryMv: number;
  outputStateMask: number;
  outputFaultMask: number;
  rssiDbm: number;
  /** Wire byte 15 bit 0 (docs/PROTOCOL.md §5/§11.4): cheat-code entry is
   * currently in backoff after repeated wrong entries. Never gates
   * phone-as-key or the ignition switch. */
  cheatcodeBackoff: boolean;
  /** Wire byte 15 bit 1 (docs/PROTOCOL.md §5/§12): the low-voltage
   * cutoff is suppressing non-essential outputs right now. Distinct from
   * outputStateMask, which reflects commanded intent, not actual driven
   * state — see mc_output_lv_cutoff_active()'s doc comment. */
  lvCutoffActive: boolean;
  /** Wire byte 15 bit 2 (docs/PROTOCOL.md §5): hazards are running right
   * now. Not derivable from outputStateMask — hazard members blink, so that
   * mask alternates several times a second and says nothing about whether
   * pressing the button will start or stop them. */
  hazardActive: boolean;
}

export function isOutputOn(status: Status, channel: number): boolean {
  return (status.outputStateMask & (1 << channel)) !== 0;
}

/** What a channel does when its trigger fires (mc_output_behaviour_t).
 * schema_version 6 replaced the old function/mode pair with this plus the
 * explicit role flags below — a rider names a channel whatever they like, and
 * firmware only needs to know the roles that carry real safety logic. */
export const OUTPUT_BEHAVIOURS = [
  'toggle',
  'momentary',
  'blink',
  'flasher',
] as const;
export type OutputBehaviour = (typeof OUTPUT_BEHAVIOURS)[number];

export const BEHAVIOUR_LABELS: Record<OutputBehaviour, string> = {
  toggle: 'On / off toggle',
  momentary: 'Momentary (while held)',
  blink: 'Blink (indicator / hazard)',
  flasher: 'Flasher (brake light)',
};

export const BEHAVIOUR_HINTS: Record<OutputBehaviour, string> = {
  toggle: 'Latches on, latches off.',
  momentary:
    'On only while its button is held. Bind it to a hold or a held chord.',
  blink: 'Flashes while on, at the turn-signal rate.',
  flasher: 'A short attention burst on switch-on, then solid.',
};

export const INDICATOR_SIDES = ['none', 'left', 'right'] as const;
export type IndicatorSide = (typeof INDICATOR_SIDES)[number];

export const INDICATOR_LABELS: Record<IndicatorSide, string> = {
  none: 'Not an indicator',
  left: 'Left indicator',
  right: 'Right indicator',
};

export interface OutputChannelConfig {
  /** Free text. Nothing in the firmware keys off a name. */
  name: string;
  behaviour: OutputBehaviour;
  commanded_on: boolean;
  /** 1-100. Below 100 dims the channel whenever it is driven on; composes
   * with toggle and momentary, never applied to blink/flasher patterns. */
  pwm_duty_pct: number;

  /* Role flags — the only channel properties that carry safety logic. */

  /** Never switched off by the low-voltage cutoff. Set on anything that must
   * not go dark mid-ride. Ignition and brake are treated as essential even
   * without this, so a config mistake can't shed them. */
  essential: boolean;
  /** The immobilizer's target, and how the lock knows the bike is running. */
  is_ignition: boolean;
  /** Never commandable from the app; inhibited while the engine runs and
   * gated behind the neutral/clutch interlock. */
  is_starter: boolean;
  /** The brake light: target of the brake-switch pass-through. */
  is_brake: boolean;
  /** Turn mutual exclusion and auto-cancel apply only to indicators. */
  indicator: IndicatorSide;
  /** Blinks together with the hazards. Anything can join — a DRL, an aux
   * light — without becoming an indicator. */
  hazard_member: boolean;
  /** Comes on with the ignition and goes off with it, like a key turned to
   * "on". Edge-triggered: the rider can still switch it off mid-ride and it
   * stays off until the next ignition cycle. */
  on_with_ignition: boolean;
  /** Channel this one alternates with (hi/lo beam, two DRL colours), or -1.
   * Must be reciprocal — the firmware rejects a one-way link. Lighting
   * either member puts the other out. */
  alternate_channel: number;
}

export type ComboType = 'chord' | 'sequence';

/* Action ids — see docs/PROTOCOL.md §9. Ids 1-3 resolve via a channel's
 * assigned `function`; OUTPUT_TOGGLE_BASE + N addresses output channel N
 * directly and is what a "bind this button to that output" UI should emit,
 * since all 12 outputs are electrically identical. */
export const ACTION_NONE = 0;
export const ACTION_TURN_L_TOGGLE = 1;
export const ACTION_TURN_R_TOGGLE = 2;
export const ACTION_HAZARD_TOGGLE = 3;
export const ACTION_OUTPUT_TOGGLE_BASE = 256;
/** ALTERNATE_BASE + N steps the pair channel N belongs to: whichever member
 * is lit, light the other. Never lands on both-off — see
 * mc_output_alternate_press(). */
export const ACTION_OUTPUT_ALTERNATE_BASE = 512;

/** Action id that toggles output channel `ch` (0-11) directly. */
export function actionToggleOutput(ch: number): number {
  return ACTION_OUTPUT_TOGGLE_BASE + ch;
}

/** Inverse of actionToggleOutput, or null if `id` isn't a direct binding. */
export function outputChannelForAction(id: number): number | null {
  const ch = id - ACTION_OUTPUT_TOGGLE_BASE;
  return ch >= 0 && ch < 12 ? ch : null;
}

/** Action id that steps the alternating pair channel `ch` belongs to. */
export function actionAlternateOutput(ch: number): number {
  return ACTION_OUTPUT_ALTERNATE_BASE + ch;
}

/** Inverse of actionAlternateOutput, or null if `id` isn't a pair binding. */
export function outputChannelForAlternateAction(id: number): number | null {
  const ch = id - ACTION_OUTPUT_ALTERNATE_BASE;
  return ch >= 0 && ch < 12 ? ch : null;
}

/** Max actions one trigger may fire (MC_ACTION_LIST_MAX). */
export const ACTION_LIST_MAX = 4;

export interface ComboDef {
  type: ComboType;
  buttons: number[];
  /** Up to ACTION_LIST_MAX ids, applied in order. Empty means unbound. */
  actions: number[];
  window_ms: number;
}

export interface InputTimingConfig {
  debounce_ms: number;
  long_press_ms: number;
  double_press_gap_ms: number;
}

export interface InputsConfig {
  timing: InputTimingConfig;
  combos: ComboDef[];
  /* One action LIST per button (8 entries, indexed by input), not one id —
   * a single press may switch several outputs. Empty array = unbound.
   * schema_version 4; the firmware still parses the v3 bare-number form,
   * but the app always writes the array form. */
  short_press_action: number[][];
  long_press_action: number[][];
  double_press_action: number[][];
  /** Rider-assigned button labels, 8 entries. Empty = unnamed. */
  names: string[];
}

export interface OutputsConfig {
  channels: OutputChannelConfig[];
  starter_interlock_input: number;
  /** Input index (0-7) assigned as the brake lever/pedal switch, or
   * -1 for none — mirrors starter_interlock_input. A level, not a press
   * event; the firmware drives the `is_brake` channel from it directly. */
  brake_switch_input: number;
  /** 0 = a turn signal never auto-cancels (manual toggle only). */
  turn_auto_cancel_ms: number;
  /** Full on+off blink cycle for a 'flash_turn'-mode channel, ms. */
  turn_flash_period_ms: number;
  /** Brake-flasher attention-pulse burst (see the 'flasher' behaviour). */
  brake_flash_pulse_count: number;
  brake_flash_pulse_on_ms: number;
  brake_flash_pulse_off_ms: number;
}

/** mc_diag_channel_config_t, as it appears in the exportable JSON config
 * (mc_config_json.c) — snake_case to match the wire JSON exactly, same as
 * OutputsConfig/InputsConfig above. This is distinct from DiagConfig below
 * (camelCase), which is the shape used by the dedicated
 * DIAG_GET_CONFIG/DIAG_SET_CONFIG binary wire ops that DiagnosticsScreen
 * actually edits through — this one only matters for a full config
 * export/import round-trip (OutputsScreen-style raw JSON editing). */
export interface DiagChannelJsonConfig {
  open_load_ma: number;
  overcurrent_ma: number;
}

export interface DiagnosticsJsonConfig {
  channels: DiagChannelJsonConfig[];
  lv_cutoff_mv: number;
  lv_cutoff_hysteresis_mv: number;
  engine_run_mv: number;
  engine_run_hysteresis_mv: number;
}

export interface DeviceConfig {
  schema_version: number;
  /** Rider-chosen board name (schema_version 8). Empty means the factory
   * default, `DEVICE_NAME` — stored empty rather than as the literal so a
   * board that was never renamed stays distinguishable from one deliberately
   * named "MOTO-CTRL", which is what lets an ownership transfer reset it. */
  device_name: string;
  outputs: OutputsConfig;
  inputs: InputsConfig;
  diagnostics: DiagnosticsJsonConfig;
}

export function defaultOutputChannel(): OutputChannelConfig {
  // mode 'on', not 'off': mode independently describes electrical behavior
  // when commanded on (plain digital vs PWM vs a flasher pattern), not the
  // commanded_on/off intent itself — see mc_output_config_default()'s
  // comment (firmware/components/core/mc_output.c). 'on' is the right
  // default for a freshly-assigned channel: ordinary digital on/off until
  // explicitly opted into something else.
  return {
    name: '',
    behaviour: 'toggle',
    commanded_on: false,
    pwm_duty_pct: 100,
    essential: false,
    is_ignition: false,
    is_starter: false,
    is_brake: false,
    indicator: 'none',
    hazard_member: false,
    on_with_ignition: false,
    alternate_channel: -1,
  };
}

/** Which behaviours make sense for a given trigger. Encodes the product
 * matrix so the binding UI can't offer a combination the firmware won't
 * honour — e.g. a momentary output bound to a single tap would never fire,
 * because momentary follows a HOLD. */
export function behavioursForTrigger(
  trigger: 'tap' | 'double' | 'hold' | 'chord' | 'switch',
): readonly OutputBehaviour[] {
  switch (trigger) {
    case 'tap':
    case 'double':
    case 'chord':
      return ['toggle', 'blink'];
    case 'hold':
      return ['momentary', 'toggle'];
    case 'switch':
      return ['momentary', 'toggle', 'blink'];
  }
}

export function defaultDiagnosticsJsonConfig(): DiagnosticsJsonConfig {
  return {
    channels: Array.from({ length: OUTPUT_COUNT }, () => ({
      open_load_ma: 50,
      overcurrent_ma: 15000,
    })),
    lv_cutoff_mv: 11800,
    lv_cutoff_hysteresis_mv: 300,
    engine_run_mv: 13800,
    engine_run_hysteresis_mv: 300,
  };
}

export function defaultConfig(): DeviceConfig {
  return {
    schema_version: 8,
    device_name: '',
    outputs: {
      channels: Array.from({ length: OUTPUT_COUNT }, defaultOutputChannel),
      starter_interlock_input: -1,
      brake_switch_input: -1,
      turn_auto_cancel_ms: 30000,
      turn_flash_period_ms: 700,
      brake_flash_pulse_count: 3,
      brake_flash_pulse_on_ms: 150,
      brake_flash_pulse_off_ms: 50,
    },
    inputs: {
      timing: { debounce_ms: 20, long_press_ms: 600, double_press_gap_ms: 350 },
      combos: [],
      short_press_action: Array.from({ length: 8 }, () => []),
      long_press_action: Array.from({ length: 8 }, () => []),
      double_press_action: Array.from({ length: 8 }, () => []),
      names: new Array(8).fill(''),
    },
    diagnostics: defaultDiagnosticsJsonConfig(),
  };
}

export interface EnrolledKey {
  slot: number;
  label: string;
}

/** The lock/immobilizer config as exposed by LOCK_GET_CONFIG /
 * LOCK_SET_CONFIG (docs/PROTOCOL.md §11.5). Never carries the cheat-code
 * itself — only whether one is set and how long it is; the device stores
 * just a salted hash and never sends it anywhere. */
export interface LockConfig {
  immobilizerEnabled: boolean;
  methodsMask: number;
  /** Input index 0-7, or -1 for "not assigned" (mc_protocol.h's 0xFF). */
  ignitionSwitchInput: number;
  autoLockGraceMs: number;
  cheatcodeWindowMs: number;
  cheatcodeSet: boolean;
  cheatcodeLen: number;
}

export function defaultLockConfig(): LockConfig {
  return {
    immobilizerEnabled: false,
    methodsMask: 0,
    ignitionSwitchInput: -1,
    autoLockGraceMs: 60000,
    cheatcodeWindowMs: 5000,
    cheatcodeSet: false,
    cheatcodeLen: 0,
  };
}

export interface Keypair {
  publicKey: Uint8Array;
  secretKey: Uint8Array;
}

/**
 * Diagnostics. Unlike lock config (which has no JSON form at all),
 * diagnostics thresholds/cutoff DO ride the exportable JSON config
 * (DiagnosticsJsonConfig above) — but DiagnosticsScreen edits live values
 * through the dedicated DIAG_GET_CONFIG/DIAG_SET_CONFIG wire ops below
 * (docs/PROTOCOL.md §12), the same pattern LockScreen already uses for
 * LOCK_GET_CONFIG/LOCK_SET_CONFIG, so this is a separate camelCase shape
 * from DiagnosticsJsonConfig, mirroring how LockConfig differs from any
 * JSON representation.
 */
export interface DiagChannelThresholds {
  openLoadMa: number;
  overcurrentMa: number;
}

export interface DiagConfig {
  channels: DiagChannelThresholds[];
  lvCutoffMv: number;
  lvCutoffHysteresisMv: number;
  engineRunMv: number;
  engineRunHysteresisMv: number;
}

export function defaultDiagConfig(): DiagConfig {
  return {
    channels: Array.from({ length: OUTPUT_COUNT }, () => ({
      openLoadMa: 50,
      overcurrentMa: 15000,
    })),
    lvCutoffMv: 11800,
    lvCutoffHysteresisMv: 300,
    engineRunMv: 13800,
    engineRunHysteresisMv: 300,
  };
}

/** Board calibration (MC_OP_DIAG_GET_CALIB/SET_CALIB). Never rides the JSON
 * config or a factory reset — see mc_diag.h / nvs_calib_hal.h. */
export interface DiagCalib {
  isGain: number;
  isOffsetMv: number;
  kilis: number;
  vbatGain: number;
  vbatOffsetMv: number;
}

export function defaultDiagCalib(): DiagCalib {
  return {
    isGain: 1,
    isOffsetMv: 0,
    kilis: 1,
    vbatGain: 11.0011,
    vbatOffsetMv: 0,
  };
}

export interface DiagChannelReading {
  currentMa: number;
  /** mc_diag_fault_t (DIAG_FAULT in constants.ts). */
  fault: number;
}

/** Live per-channel readings (MC_OP_DIAG_GET). */
export interface Diagnostics {
  channels: DiagChannelReading[];
}

/** OTA_STATUS_RESULT (docs/PROTOCOL.md §10). `state` is OTA_STATE in constants.ts. */
export interface OtaStatus {
  state: number;
  bytesReceived: number;
  imageSize: number;
}

/** One EVENT_LOG_CHUNK record (docs/PROTOCOL.md §15, mc_event_record_t). */
export interface EventRecord {
  seq: number;
  uptimeMs: number;
  /** mc_event_type_t (EVENT_TYPE in constants.ts). */
  type: number;
  arg0: number;
  arg1: number;
}

/** A parsed firmware bundle, ready to hand to MotoClient.uploadFirmware() —
 * either from a downloaded `.mcota` file (parseMcotaBundle in
 * updateCheck.ts) or synthesized in a test. */
export interface FirmwareBundle {
  imageSize: number;
  sha512: Uint8Array;
  signature: Uint8Array;
  image: Uint8Array;
}

/** The update-check manifest (docs/PROTOCOL.md §10.5), fetched from the
 * baked-in UPDATE_MANIFEST_URL in protocol/constants.ts. */
export interface UpdateManifest {
  version: string;
  changelog: string;
  bundle_url: string;
  bundle_sha512: string;
  bundle_size: number;
}
