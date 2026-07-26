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
}

export function isOutputOn(status: Status, channel: number): boolean {
  return (status.outputStateMask & (1 << channel)) !== 0;
}

export const OUTPUT_FUNCTIONS = [
  'none',
  'headlight_hi',
  'headlight_lo',
  'brake',
  'turn_l',
  'turn_r',
  'horn',
  'ignition',
  'starter',
  'aux',
] as const;
export type OutputFunction = (typeof OUTPUT_FUNCTIONS)[number];

/** mc_output_mode_t: 'off'/'on'/'pwm'/'flash_turn'/'flash_brake'. 'off'/'on'
 * are plain digital; 'pwm' is a steady dimmed brightness at pwmDutyPct
 * while commanded on (opt-in, off by default per AGENTS.md's PWM/flasher
 * rule); 'flash_turn' blinks at outputs.turn_flash_period_ms; 'flash_brake'
 * plays an attention-pulse burst (outputs.brake_flash_pulse_*) on the
 * off->on transition, then solid — also opt-in/off-by-default (AGENTS.md
 * #5: flash patterns aren't legal everywhere), so a BRAKE-function channel
 * defaults to plain 'on', not 'flash_brake'. */
export type OutputMode = 'off' | 'on' | 'pwm' | 'flash_turn' | 'flash_brake';

export interface OutputChannelConfig {
  function: OutputFunction;
  name: string;
  mode: OutputMode;
  commanded_on: boolean;
  /** 1-100, meaningful only when mode === 'pwm'. */
  pwm_duty_pct: number;
}

export type ComboType = 'chord' | 'sequence';

export interface ComboDef {
  type: ComboType;
  buttons: number[];
  window_ms: number;
  action_id: number;
}

export interface InputTimingConfig {
  debounce_ms: number;
  long_press_ms: number;
  double_press_gap_ms: number;
}

export interface InputsConfig {
  timing: InputTimingConfig;
  combos: ComboDef[];
  short_press_action: number[];
  long_press_action: number[];
  double_press_action: number[];
}

export interface OutputsConfig {
  channels: OutputChannelConfig[];
  starter_interlock_input: number;
  /** Input index (0-7) assigned as the brake lever/pedal switch, or
   * -1 for none — mirrors starter_interlock_input. A level, not a press
   * event; the firmware drives the BRAKE-function channel from it directly. */
  brake_switch_input: number;
  /** 0 = a turn signal never auto-cancels (manual toggle only). */
  turn_auto_cancel_ms: number;
  /** Full on+off blink cycle for a 'flash_turn'-mode channel, ms. */
  turn_flash_period_ms: number;
  /** Brake-flasher attention-pulse burst (see OutputMode's doc comment). */
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
 * export/import round-trip (PinMapperScreen-style raw JSON editing). */
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
  return { function: 'none', name: '', mode: 'on', commanded_on: false, pwm_duty_pct: 100 };
}

export function defaultDiagnosticsJsonConfig(): DiagnosticsJsonConfig {
  return {
    channels: Array.from({ length: OUTPUT_COUNT }, () => ({ open_load_ma: 50, overcurrent_ma: 15000 })),
    lv_cutoff_mv: 11800,
    lv_cutoff_hysteresis_mv: 300,
    engine_run_mv: 13800,
    engine_run_hysteresis_mv: 300,
  };
}

export function defaultConfig(): DeviceConfig {
  return {
    schema_version: 3,
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
      short_press_action: new Array(8).fill(0),
      long_press_action: new Array(8).fill(0),
      double_press_action: new Array(8).fill(0),
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
    channels: Array.from({ length: OUTPUT_COUNT }, () => ({ openLoadMa: 50, overcurrentMa: 15000 })),
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
  return { isGain: 1, isOffsetMv: 0, kilis: 1, vbatGain: 11.0011, vbatOffsetMv: 0 };
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

/** The update-check manifest (docs/PROTOCOL.md §10.5) fetched from
 * AGENTS.md's baked-in update-check URL. */
export interface UpdateManifest {
  version: string;
  changelog: string;
  bundle_url: string;
  bundle_sha512: string;
  bundle_size: number;
}
