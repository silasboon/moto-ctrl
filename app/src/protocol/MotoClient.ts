/**
 * High-level MOTO-CTRL protocol client: sits on top of a Transport
 * (BlePlxTransport or SimTransport) and exposes the real wire protocol
 * (docs/PROTOCOL.md) as typed async methods. Mirrors the flows already
 * proven against the simulator in firmware/sim/gui/app.js and
 * firmware/sim/itest/moto-client.mjs.
 *
 * Status notifications: real firmware's mc_session only replies to an
 * explicit STATUS_GET — it does not (yet) broadcast on state change, unlike
 * the simulator's dev-convenience ticker. This client polls
 * status on an interval while connected, and immediately after every
 * command, so the UI still feels live. A future firmware enhancement could
 * push on change; not built here.
 */
import nacl from 'tweetnacl';

import type { Transport } from '../transport/Transport';
import {
  AUTH_CONTEXT,
  CONFIG_CHUNK_BYTES,
  CONFIG_JSON_MAX,
  MC_CH,
  MC_OP,
  MC_RESULT,
  OTA_CHUNK_BYTES,
  OUTPUT_COUNT,
  resultName,
} from './constants';
import {
  concatBytes,
  f32le,
  i16le,
  readF32le,
  readI16le,
  readU16le,
  readU32le,
  u16le,
  u32le,
  utf8Decode,
  utf8Encode,
} from './frames';
import type {
  DeviceConfig,
  DiagCalib,
  DiagConfig,
  Diagnostics,
  EnrolledKey,
  EventRecord,
  FirmwareBundle,
  Keypair,
  LockConfig,
  OtaStatus,
  Status,
} from './types';

export interface ResultOutcome {
  ok: boolean;
  result: number;
  resultName: string;
}

/** A button press reported by the device while learn mode is on
 * (docs/PROTOCOL.md §14.1). */
export interface InputEvent {
  /** Input index, 0-7. */
  button: number;
  pressType: 'short' | 'long' | 'double';
  /** True when a chord consumed this press, so its own binding didn't fire. */
  actionSuppressed: boolean;
}

const PRESS_TYPE_NAMES: InputEvent['pressType'][] = ['short', 'long', 'double'];

export interface AuthOutcome extends ResultOutcome {
  slot: number;
}

const STATUS_POLL_MS = 1500;
const REPLY_TIMEOUT_MS = 5000;

function parseStatus(b: Uint8Array): Status {
  return {
    fwMajor: b[0]!,
    fwMinor: b[1]!,
    fwPatch: b[2]!,
    lockState: b[3]!,
    uptimeMs: readU32le(b, 4),
    batteryMv: readU16le(b, 8),
    outputStateMask: readU16le(b, 10),
    outputFaultMask: readU16le(b, 12),
    rssiDbm: (b[14]! << 24) >> 24,
    cheatcodeBackoff: (b[15]! & 0x01) !== 0,
    lvCutoffActive: (b[15]! & 0x02) !== 0,
    hazardActive: (b[15]! & 0x04) !== 0,
  };
}

function outcome(result: number): ResultOutcome {
  return {
    ok: result === MC_RESULT.OK,
    result,
    resultName: resultName(result),
  };
}

interface PendingReply {
  resolve: (frame: { op: number; body: Uint8Array }) => void;
  reject: (err: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class MotoClient {
  private status: Status | null = null;
  private authedSlot = -1;
  private pending = new Map<number, PendingReply>();
  private statusListeners = new Set<(status: Status) => void>();
  private inputEventListeners = new Set<(event: InputEvent) => void>();
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private unsubscribeMessage: (() => void) | null = null;
  private unsubscribeConnState: (() => void) | null = null;

  constructor(private readonly transport: Transport) {}

  async connect(deviceId: string): Promise<void> {
    this.unsubscribeMessage?.();
    this.unsubscribeMessage = this.transport.onMessage((channel, data) =>
      this.handleMessage(channel, data),
    );
    this.unsubscribeConnState = this.transport.onConnectionStateChange(
      state => {
        if (state !== 'connected') {
          this.stopPolling();
          this.authedSlot = -1;
          /* Fail every in-flight request immediately, with a reason. Without
           * this they sit armed until REPLY_TIMEOUT_MS and then reject with a
           * bare "timed out", which reads as a protocol fault rather than
           * "the board went away" — and, if the caller navigated on, surfaces
           * as an unhandled rejection seconds after the fact. */
          this.failPending(new Error('MotoClient: the board disconnected'));
        }
      },
    );
    await this.transport.connect(deviceId);
    this.startPolling();
  }

  async disconnect(): Promise<void> {
    this.stopPolling();
    this.unsubscribeMessage?.();
    this.unsubscribeConnState?.();
    this.authedSlot = -1;
    await this.transport.disconnect();
  }

  getConnectionState() {
    return this.transport.getConnectionState();
  }

  onConnectionStateChange(
    listener: (state: ReturnType<Transport['getConnectionState']>) => void,
  ): () => void {
    return this.transport.onConnectionStateChange(listener);
  }

  /** Phone-side RSSI of the live connection — see Transport.readRssi. Null
   * on transports that don't support it (the sim) or with nothing to read. */
  async readRssi(): Promise<number | null> {
    return (await this.transport.readRssi?.()) ?? null;
  }

  getLastStatus(): Status | null {
    return this.status;
  }

  onStatus(listener: (status: Status) => void): () => void {
    this.statusListeners.add(listener);
    return () => this.statusListeners.delete(listener);
  }

  isAuthenticated(): boolean {
    return this.authedSlot >= 0;
  }

  async getStatus(): Promise<Status> {
    const reply = await this.request(
      MC_CH.STATUS,
      MC_OP.STATUS_GET,
      new Uint8Array(0),
    );
    const status = parseStatus(reply.body);
    this.status = status;
    for (const l of this.statusListeners) l(status);
    return status;
  }

  /** Full challenge-response flow (docs/PROTOCOL.md §6). Registers each
   * reply waiter before sending the request that triggers it — a transport
   * that ever replied synchronously within send() would otherwise have its
   * reply dropped (no pending waiter registered yet to catch it). */
  async authenticate(keypair: Keypair): Promise<AuthOutcome> {
    const challengePromise = this.waitForReply(MC_CH.AUTH);
    await this.send(MC_CH.AUTH, MC_OP.AUTH_BEGIN, new Uint8Array(0));
    const challenge = await challengePromise;
    if (challenge.op !== MC_OP.AUTH_CHALLENGE) {
      throw new Error(
        `MotoClient.authenticate: unexpected reply op 0x${challenge.op.toString(16)}`,
      );
    }
    const nonce = challenge.body;
    const message = concatBytes(utf8Encode(AUTH_CONTEXT), nonce);
    const signature = nacl.sign.detached(message, keypair.secretKey);

    const resultPromise = this.waitForReply(MC_CH.AUTH);
    await this.send(MC_CH.AUTH, MC_OP.AUTH_RESPONSE, signature);
    const result = await resultPromise;
    const resultCode = result.body[0]!;
    const slot = result.body[1]!;
    if (resultCode === MC_RESULT.OK) {
      this.authedSlot = slot;
    }
    return { ...outcome(resultCode), slot };
  }

  /** Enrolls a public key (this phone's own, for first pairing/TOFU, or
   * another phone's — shared out-of-band — once this session is already
   * authenticated; docs/PROTOCOL.md §6). Only ever needs a public key,
   * never a full Keypair: this device never has another phone's private
   * key, by design (phone-as-key auth). */
  async enroll(publicKey: Uint8Array, label: string): Promise<AuthOutcome> {
    const labelBytes = utf8Encode(label).subarray(0, 23);
    const reply = await this.request(
      MC_CH.AUTH,
      MC_OP.ENROLL,
      concatBytes(publicKey, labelBytes),
    );
    const resultCode = reply.body[0]!;
    const slot = reply.body[1]!;
    return { ...outcome(resultCode), slot };
  }

  async keyList(): Promise<EnrolledKey[]> {
    const reply = await this.request(
      MC_CH.AUTH,
      MC_OP.KEY_LIST,
      new Uint8Array(0),
    );
    const count = reply.body[0]!;
    const keys: EnrolledKey[] = [];
    let pos = 1;
    for (let i = 0; i < count; i++) {
      const slot = reply.body[pos]!;
      const labelLen = reply.body[pos + 1]!;
      const label = utf8Decode(
        reply.body.subarray(pos + 2, pos + 2 + labelLen),
      );
      keys.push({ slot, label });
      pos += 2 + labelLen;
    }
    return keys;
  }

  async keyRevoke(slot: number): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.AUTH,
      MC_OP.KEY_REVOKE,
      new Uint8Array([slot]),
    );
    return outcome(reply.body[0]!);
  }

  async setOutput(channel: number, on: boolean): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.SET_OUTPUT,
      new Uint8Array([channel, on ? 1 : 0]),
    );
    const result = outcome(reply.body[1]!);
    // Refresh status right after a command so the UI reflects the change
    // without waiting for the next poll tick.
    this.getStatus().catch(() => {});
    return result;
  }

  /** Toggles hazards — both indicator sides together, bypassing
   * the mutual exclusion a plain setOutput() on a turn channel applies
   * device-side, and arming no auto-cancel timer (hazards stay on until
   * pressed again). REJECTED if neither channel is configured. */
  async hazardPress(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.HAZARD_PRESS,
      new Uint8Array(0),
    );
    const result = outcome(reply.body[1]!);
    this.getStatus().catch(() => {});
    return result;
  }

  /** Subscribes to button presses pushed while learn mode is on. Returns an
   * unsubscribe function. Listening alone does nothing — call
   * `inputLearn(true)` to make the device start sending. */
  onInputEvent(listener: (event: InputEvent) => void): () => void {
    this.inputEventListeners.add(listener);
    return () => this.inputEventListeners.delete(listener);
  }

  /**
   * Turns button-identification learn mode on or off for this session
   * (docs/PROTOCOL.md §14.1). Off by default; the device also drops it on
   * disconnect, so there's no risk of leaving it on across a reconnect.
   *
   * `suppressActions` additionally stops the device running the handlebar
   * bindings for the presses it reports — for capturing a cheat-code, where
   * the rider is pressing whichever buttons make up their code and would
   * otherwise sound the horn once per press. Handlebar controls are inert
   * while it holds, so only ask for it in a mode the rider deliberately
   * entered. The brake light and the cheat-code matcher itself are never
   * suppressed.
   */
  async inputLearn(
    enable: boolean,
    suppressActions = false,
  ): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.INPUT_LEARN,
      new Uint8Array([enable ? 1 : 0, suppressActions ? 1 : 0]),
    );
    return outcome(reply.body[1]!);
  }

  async configRead(): Promise<DeviceConfig> {
    const bytes = await this.readConfigBytes();
    return JSON.parse(utf8Decode(bytes)) as DeviceConfig;
  }

  /** Accumulates CONFIG_CHUNK frames (docs/PROTOCOL.md §8) until `total`
   * bytes are collected. Reassembles by offset, since chunks are not
   * guaranteed to arrive in order. */
  private readConfigBytes(): Promise<Uint8Array> {
    const promise = new Promise<Uint8Array>((resolve, reject) => {
      const chunks = new Map<number, Uint8Array>();
      let expectedTotal = 0;
      /* Report how much arrived, not just that time ran out. "no chunks at
       * all" (device never answered) and "1920 of 2250 bytes" (chunks were
       * dropped in flight) have completely different causes, and the bare
       * "timed out" hid that distinction during the msys-pool-exhaustion bug
       * — see ble_send() in firmware/main/ble/gatt_svr.c. */
      const timer = setTimeout(() => {
        this.pending.delete(MC_CH.CONFIG);
        const got = [...chunks.values()].reduce((n, d) => n + d.length, 0);
        reject(
          new Error(
            expectedTotal === 0
              ? 'MotoClient.configRead: timed out with no response from the device'
              : `MotoClient.configRead: timed out after ${got} of ${expectedTotal} bytes ` +
                  `(${chunks.size} chunk(s) received; some were dropped in flight)`,
          ),
        );
      }, REPLY_TIMEOUT_MS);
      const fail = (err: Error) => {
        clearTimeout(timer);
        this.pending.delete(MC_CH.CONFIG);
        reject(err);
      };

      this.pending.set(MC_CH.CONFIG, {
        resolve: frame => {
          if (frame.op !== MC_OP.CONFIG_CHUNK) {
            fail(
              new Error(
                `MotoClient.configRead: unexpected reply op 0x${frame.op.toString(16)}`,
              ),
            );
            return;
          }
          const total = readU16le(frame.body, 2);
          if (total === 0) {
            fail(
              new Error('MotoClient.configRead: device reported empty config'),
            );
            return;
          }
          expectedTotal = total;
          const offset = readU16le(frame.body, 0);
          chunks.set(offset, frame.body.subarray(4));
          const got = [...chunks.values()].reduce((n, d) => n + d.length, 0);
          if (got >= total) {
            clearTimeout(timer);
            this.pending.delete(MC_CH.CONFIG);
            const buf = new Uint8Array(total);
            for (const [off, d] of chunks) buf.set(d, off);
            resolve(buf);
          }
          // else: entry stays armed in `this.pending` for the next chunk.
        },
        reject: fail,
        timer,
      });

      this.send(MC_CH.CONFIG, MC_OP.CONFIG_READ, new Uint8Array(0)).catch(err =>
        fail(err as Error),
      );
    });

    /* Same reason as waitForReply(): armed before the caller awaits, so a
     * disconnect landing in that gap would otherwise read as unhandled. */
    promise.catch(() => {});
    return promise;
  }

  async configWrite(config: DeviceConfig): Promise<ResultOutcome> {
    const json = JSON.stringify(config);
    const bytes = utf8Encode(json);
    if (bytes.length === 0 || bytes.length > CONFIG_JSON_MAX) {
      throw new Error(
        `MotoClient.configWrite: ${bytes.length} bytes exceeds CONFIG_JSON_MAX (${CONFIG_JSON_MAX})`,
      );
    }
    await this.send(
      MC_CH.CONFIG,
      MC_OP.CONFIG_WRITE_BEGIN,
      u16le(bytes.length),
    );
    for (let off = 0; off < bytes.length; off += CONFIG_CHUNK_BYTES) {
      const chunk = bytes.subarray(
        off,
        Math.min(off + CONFIG_CHUNK_BYTES, bytes.length),
      );
      await this.send(
        MC_CH.CONFIG,
        MC_OP.CONFIG_WRITE_CHUNK,
        concatBytes(u16le(off), chunk),
      );
    }
    const reply = await this.request(
      MC_CH.CONFIG,
      MC_OP.CONFIG_WRITE_COMMIT,
      new Uint8Array(0),
    );
    return outcome(reply.body[0]!);
  }

  // --- OTA (docs/PROTOCOL.md §10) ---

  /** Verifies `signature` over `sha512` and, if it checks out and the
   * device is in a safe state (not riding, battery not critical), starts
   * accepting OTA_CHUNKs. `sha512`/`signature` normally come from a parsed
   * `.mcota` bundle (updateCheck.ts's parseMcotaBundle) — see
   * FirmwareBundle. Prefer uploadFirmware() below over calling this
   * directly; it drives the whole begin/chunk/commit sequence. */
  async otaBegin(
    imageSize: number,
    sha512: Uint8Array,
    signature: Uint8Array,
  ): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_BEGIN,
      concatBytes(u32le(imageSize), sha512, signature),
    );
    return outcome(reply.body[0]!);
  }

  async otaChunk(offset: number, data: Uint8Array): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_CHUNK,
      concatBytes(u32le(offset), data),
    );
    return outcome(reply.body[0]!);
  }

  async otaCommit(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_COMMIT,
      new Uint8Array(0),
    );
    return outcome(reply.body[0]!);
  }

  /** Idempotent — safe to call from any OTA state, including IDLE. */
  async otaAbort(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_ABORT,
      new Uint8Array(0),
    );
    return outcome(reply.body[0]!);
  }

  /** Applies a COMMITTED image now. Re-checks the same safe-state gate as
   * otaBegin() (docs/PROTOCOL.md §10.3) — REJECTED leaves the committed
   * image intact for a later retry, it does not lose the transfer. */
  async otaReboot(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_REBOOT,
      new Uint8Array(0),
    );
    return outcome(reply.body[0]!);
  }

  async otaStatus(): Promise<OtaStatus & ResultOutcome> {
    const reply = await this.request(
      MC_CH.OTA,
      MC_OP.OTA_STATUS,
      new Uint8Array(0),
    );
    const b = reply.body;
    return {
      ...outcome(b[0]!),
      state: b[1]!,
      bytesReceived: readU32le(b, 2),
      imageSize: readU32le(b, 6),
    };
  }

  /** Drives the full begin -> chunk* -> commit sequence for a parsed
   * firmware bundle, stopping at the first rejected step. Does not call
   * otaReboot() — applying the update is a separate, explicit action so
   * the UI can let the rider choose when (docs/PROTOCOL.md §10.1: a
   * COMMITTED image is safe to leave sitting until then). */
  async uploadFirmware(
    bundle: FirmwareBundle,
    onProgress?: (sentBytes: number, totalBytes: number) => void,
  ): Promise<ResultOutcome> {
    const begin = await this.otaBegin(
      bundle.imageSize,
      bundle.sha512,
      bundle.signature,
    );
    if (!begin.ok) {
      return begin;
    }
    for (let off = 0; off < bundle.image.length; off += OTA_CHUNK_BYTES) {
      const chunk = bundle.image.subarray(
        off,
        Math.min(off + OTA_CHUNK_BYTES, bundle.image.length),
      );
      const result = await this.otaChunk(off, chunk);
      if (!result.ok) {
        return result;
      }
      onProgress?.(off + chunk.length, bundle.image.length);
    }
    return this.otaCommit();
  }

  // --- Event log (docs/PROTOCOL.md §15) ---

  /** Fetches every record with seq > sinceSeq (0 = everything retained),
   * oldest-first. Rides the COMMAND channel like SET_OUTPUT/lock/
   * diagnostics — don't call this concurrently with another COMMAND-channel
   * request (same one-in-flight-per-channel constraint readConfigBytes has
   * on the CONFIG channel above). */
  getEventLog(sinceSeq = 0): Promise<EventRecord[]> {
    const promise = new Promise<EventRecord[]>((resolve, reject) => {
      const records: EventRecord[] = [];
      let total: number | null = null;
      const timer = setTimeout(() => {
        this.pending.delete(MC_CH.COMMAND);
        reject(new Error('MotoClient.getEventLog: timed out'));
      }, REPLY_TIMEOUT_MS);
      const fail = (err: Error) => {
        clearTimeout(timer);
        this.pending.delete(MC_CH.COMMAND);
        reject(err);
      };

      this.pending.set(MC_CH.COMMAND, {
        resolve: frame => {
          if (frame.op !== MC_OP.EVENT_LOG_CHUNK) {
            fail(
              new Error(
                `MotoClient.getEventLog: unexpected reply op 0x${frame.op.toString(16)}`,
              ),
            );
            return;
          }
          total = readU16le(frame.body, 2);
          const count = frame.body[4]!;
          let pos = 5;
          for (let i = 0; i < count; i++) {
            const seq = readU32le(frame.body, pos);
            pos += 4;
            const uptimeMs = readU32le(frame.body, pos);
            pos += 4;
            const type = frame.body[pos++]!;
            const arg0 = frame.body[pos++]!;
            const arg1 = frame.body[pos++]!;
            pos += 1; // reserved
            records.push({ seq, uptimeMs, type, arg0, arg1 });
          }
          if (records.length >= (total ?? 0)) {
            clearTimeout(timer);
            this.pending.delete(MC_CH.COMMAND);
            resolve(records);
          }
          // else: entry stays armed in `this.pending` for the next chunk.
        },
        reject: fail,
        timer,
      });

      this.send(MC_CH.COMMAND, MC_OP.EVENT_LOG_GET, u32le(sinceSeq)).catch(
        err => fail(err as Error),
      );
    });

    /* Same reason as waitForReply(): armed before the caller awaits, so a
     * disconnect landing in that gap would otherwise read as unhandled. */
    promise.catch(() => {});
    return promise;
  }

  // --- Lock / immobilizer (docs/PROTOCOL.md §11) ---

  /** Locks now if the immobilizer is enabled and the guard
   * (!engine_running && !ignition_live) holds; REJECTED otherwise.
   * Idempotent if already LOCKED. */
  async lock(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.LOCK,
      new Uint8Array(0),
    );
    const result = outcome(reply.body[1]!);
    this.getStatus().catch(() => {});
    return result;
  }

  /** Explicit phone-as-key unlock. REJECTED if the PHONE method isn't
   * enabled; a no-op OK if not currently LOCKED. Auto-unlock on a fresh
   * authentication (docs/PROTOCOL.md §11.2) happens device-side and needs
   * no app action — this is for a session that was already authenticated
   * before the bike became LOCKED. */
  async unlock(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.UNLOCK,
      new Uint8Array(0),
    );
    const result = outcome(reply.body[1]!);
    this.getStatus().catch(() => {});
    return result;
  }

  async lockGetConfig(): Promise<LockConfig> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.LOCK_GET_CONFIG,
      new Uint8Array(0),
    );
    const b = reply.body;
    return {
      immobilizerEnabled: b[0] !== 0,
      methodsMask: b[1]!,
      ignitionSwitchInput: b[2] === 0xff ? -1 : b[2]!,
      autoLockGraceMs: readU16le(b, 3),
      cheatcodeWindowMs: readU16le(b, 5),
      cheatcodeSet: b[7] !== 0,
      cheatcodeLen: b[8]!,
    };
  }

  /** Applies the non-cheatcode lock config fields (immobilizerEnabled,
   * methodsMask, ignitionSwitchInput, autoLockGraceMs, cheatcodeWindowMs).
   * Leaves the cheat-code itself untouched — set/clear it separately.
   * REJECTED if enabling without a cheat-code already set, or without an
   * `ignition`-function output channel configured, or if the
   * ignition-switch method is enabled with no valid input assigned. */
  async lockSetConfig(cfg: {
    immobilizerEnabled: boolean;
    methodsMask: number;
    ignitionSwitchInput: number;
    autoLockGraceMs: number;
    cheatcodeWindowMs: number;
  }): Promise<ResultOutcome> {
    const body = new Uint8Array(7);
    body[0] = cfg.immobilizerEnabled ? 1 : 0;
    body[1] = cfg.methodsMask & 0xff;
    body[2] = cfg.ignitionSwitchInput < 0 ? 0xff : cfg.ignitionSwitchInput;
    body.set(u16le(cfg.autoLockGraceMs), 3);
    body.set(u16le(cfg.cheatcodeWindowMs), 5);
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.LOCK_SET_CONFIG,
      body,
    );
    return outcome(reply.body[1]!);
  }

  /** Sets a new cheat-code (4-10 button indices 0-7). The device hashes +
   * salts it — the plaintext sequence never round-trips back over the
   * wire, only this one write. */
  async cheatcodeSet(buttons: number[]): Promise<ResultOutcome> {
    const body = concatBytes(
      new Uint8Array([buttons.length]),
      new Uint8Array(buttons),
    );
    const reply = await this.request(MC_CH.COMMAND, MC_OP.CHEATCODE_SET, body);
    return outcome(reply.body[1]!);
  }

  /** REJECTED while the immobilizer is enabled (layered unlock: the
   * mandatory fallback can't be removed out from under an active
   * immobilizer) — disable it first. */
  async cheatcodeClear(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.CHEATCODE_CLEAR,
      new Uint8Array(0),
    );
    return outcome(reply.body[1]!);
  }

  /** Practice mode: pure hash comparison against a candidate sequence, no
   * side effects (never touches the physical entry buffer or the
   * wrong-entry backoff counter). */
  async cheatcodeTest(
    buttons: number[],
  ): Promise<{ ok: boolean; match: boolean }> {
    const body = concatBytes(
      new Uint8Array([buttons.length]),
      new Uint8Array(buttons),
    );
    const reply = await this.request(MC_CH.COMMAND, MC_OP.CHEATCODE_TEST, body);
    return { ok: reply.body[0] === MC_RESULT.OK, match: reply.body[1] !== 0 };
  }

  /** Wipes every enrolled key and resets the lock config to factory
   * defaults (immobilizer disabled, no cheat-code) in one atomic op,
   * releasing any active immobilize. Requires this session to already be
   * authenticated. Does not disconnect — the app should treat this as
   * "pairing state reset" and return to the Pairing screen. */
  async transferOwnership(): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.TRANSFER_OWNERSHIP,
      new Uint8Array(0),
    );
    const result = outcome(reply.body[1]!);
    this.getStatus().catch(() => {});
    return result;
  }

  // --- Diagnostics (docs/PROTOCOL.md §12) ---

  /** Live per-channel current + fault, as of mc_diag's last round-robin
   * sample of each channel (real hardware/sim-computed, not the app's own
   * math). A channel that isn't actually energized (commanded off, or
   * suppressed by the low-voltage cutoff) always reads 0mA / no fault. */
  async getDiagnostics(): Promise<Diagnostics> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.DIAG_GET,
      new Uint8Array(0),
    );
    const b = reply.body;
    const channels = [];
    let pos = 1;
    const currents: number[] = [];
    for (let c = 0; c < OUTPUT_COUNT; c++) {
      currents.push(readU16le(b, pos));
      pos += 2;
    }
    for (let c = 0; c < OUTPUT_COUNT; c++) {
      channels.push({ currentMa: currents[c]!, fault: b[pos]! });
      pos += 1;
    }
    return { channels };
  }

  async diagGetConfig(): Promise<DiagConfig> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.DIAG_GET_CONFIG,
      new Uint8Array(0),
    );
    const b = reply.body;
    const channels = [];
    let pos = 1;
    for (let c = 0; c < OUTPUT_COUNT; c++) {
      const openLoadMa = readU16le(b, pos);
      pos += 2;
      const overcurrentMa = readU16le(b, pos);
      pos += 2;
      channels.push({ openLoadMa, overcurrentMa });
    }
    const lvCutoffMv = readU16le(b, pos);
    pos += 2;
    const lvCutoffHysteresisMv = readU16le(b, pos);
    pos += 2;
    const engineRunMv = readU16le(b, pos);
    pos += 2;
    const engineRunHysteresisMv = readU16le(b, pos);
    pos += 2;
    const engineRunVoltageDetectionEnabled = b[pos]! !== 0;
    return {
      channels,
      lvCutoffMv,
      lvCutoffHysteresisMv,
      engineRunMv,
      engineRunHysteresisMv,
      engineRunVoltageDetectionEnabled,
    };
  }

  /** REJECTED if any channel's openLoadMa >= overcurrentMa (would never be
   * classifiable as OVERCURRENT). Also updates the device's persisted
   * config (mc_config_t.diagnostics), so a full config export/import
   * captures these thresholds too. */
  async diagSetConfig(cfg: DiagConfig): Promise<ResultOutcome> {
    const body = new Uint8Array(OUTPUT_COUNT * 4 + 8 + 1);
    let pos = 0;
    for (let c = 0; c < OUTPUT_COUNT; c++) {
      body.set(u16le(cfg.channels[c]!.openLoadMa), pos);
      pos += 2;
      body.set(u16le(cfg.channels[c]!.overcurrentMa), pos);
      pos += 2;
    }
    body.set(u16le(cfg.lvCutoffMv), pos);
    pos += 2;
    body.set(u16le(cfg.lvCutoffHysteresisMv), pos);
    pos += 2;
    body.set(u16le(cfg.engineRunMv), pos);
    pos += 2;
    body.set(u16le(cfg.engineRunHysteresisMv), pos);
    pos += 2;
    body[pos] = cfg.engineRunVoltageDetectionEnabled ? 1 : 0;
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.DIAG_SET_CONFIG,
      body,
    );
    return outcome(reply.body[1]!);
  }

  async diagGetCalib(): Promise<DiagCalib> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.DIAG_GET_CALIB,
      new Uint8Array(0),
    );
    const b = reply.body;
    return {
      isGain: readF32le(b, 1),
      isOffsetMv: readI16le(b, 5),
      kilis: readF32le(b, 7),
      vbatGain: readF32le(b, 11),
      vbatOffsetMv: readI16le(b, 15),
    };
  }

  /** Board-specific analog calibration — never rides a config export/import
   * and is not cleared by transferOwnership() (see DiagCalib's doc
   * comment). A bench/installer action, not a routine one. */
  async diagSetCalib(calib: DiagCalib): Promise<ResultOutcome> {
    const body = concatBytes(
      f32le(calib.isGain),
      i16le(calib.isOffsetMv),
      f32le(calib.kilis),
      f32le(calib.vbatGain),
      i16le(calib.vbatOffsetMv),
    );
    const reply = await this.request(MC_CH.COMMAND, MC_OP.DIAG_SET_CALIB, body);
    return outcome(reply.body[1]!);
  }

  /** Samples `channel`'s current live and sets its open-load threshold from
   * roughly half the measured healthy draw. REJECTED if that channel isn't
   * actually energized right now. Pass DIAG_LEARN_ALL (constants.ts) to
   * learn every currently-energized channel at once. */
  async diagLearn(channel: number): Promise<ResultOutcome> {
    const reply = await this.request(
      MC_CH.COMMAND,
      MC_OP.DIAG_LEARN,
      new Uint8Array([channel]),
    );
    return outcome(reply.body[1]!);
  }

  // --- internals ---

  private startPolling(): void {
    this.stopPolling();
    this.getStatus().catch(() => {});
    this.pollTimer = setInterval(() => {
      this.getStatus().catch(() => {});
    }, STATUS_POLL_MS);
  }

  /** Rejects and clears every armed reply waiter. Each entry's own reject
   * clears its timer and removes it from `pending`, so this iterates a copy. */
  private failPending(err: Error): void {
    const entries = [...this.pending.values()];
    this.pending.clear();
    for (const entry of entries) {
      clearTimeout(entry.timer);
      entry.reject(err);
    }
  }

  private stopPolling(): void {
    if (this.pollTimer) {
      clearInterval(this.pollTimer);
      this.pollTimer = null;
    }
  }

  private handleMessage(channel: number, data: Uint8Array): void {
    if (data.length < 1) {
      return;
    }
    const op = data[0]!;
    const body = data.subarray(1);

    /* INPUT_EVENT is the one genuinely unsolicited device-to-client frame
     * (docs/PROTOCOL.md §14.1): it arrives whenever the rider presses a
     * button, with no request behind it. It must be routed to listeners and
     * must NOT fall through to the pending-reply resolver below — otherwise a
     * press landing between a request and its reply would resolve the wrong
     * promise, and e.g. setOutput() would parse a press event as its
     * COMMAND_RESULT. */
    if (channel === MC_CH.COMMAND && op === MC_OP.INPUT_EVENT) {
      if (body.length >= 3) {
        const event: InputEvent = {
          button: body[0]!,
          pressType: PRESS_TYPE_NAMES[body[1]!] ?? 'short',
          actionSuppressed: body[2] !== 0,
        };
        for (const listener of this.inputEventListeners) {
          listener(event);
        }
      }
      return;
    }

    const pending = this.pending.get(channel);
    if (pending) {
      pending.resolve({ op, body });
    }
  }

  private send(
    channel: number,
    opcode: number,
    payload: Uint8Array,
  ): Promise<void> {
    const frame = new Uint8Array(payload.length + 1);
    frame[0] = opcode;
    frame.set(payload, 1);
    return this.transport.send(channel, frame);
  }

  private waitForReply(
    channel: number,
    timeoutMs = REPLY_TIMEOUT_MS,
  ): Promise<{ op: number; body: Uint8Array }> {
    const promise = new Promise<{ op: number; body: Uint8Array }>(
      (resolve, reject) => {
        const timer = setTimeout(() => {
          this.pending.delete(channel);
          reject(
            new Error(
              `MotoClient: timed out waiting for a reply on channel ${channel}`,
            ),
          );
        }, timeoutMs);
        this.pending.set(channel, {
          resolve: frame => {
            clearTimeout(timer);
            this.pending.delete(channel);
            resolve(frame);
          },
          reject,
          timer,
        });
      },
    );

    /* Mark the promise handled the instant it exists.
     *
     * Callers create the waiter and THEN `await this.send(...)` before
     * awaiting the waiter itself — see request() and authenticate(). During
     * that gap the promise has no rejection handler, so a disconnect landing
     * there (failPending runs synchronously inside the transport's
     * disconnect listener) rejects a promise nobody is listening to, and the
     * runtime reports an unhandled rejection even though the caller is about
     * to await it a microtask later.
     *
     * Attaching a no-op handler does not swallow anything: `promise` is still
     * what we return, so whoever awaits it still receives the rejection. It
     * only stops the runtime treating the gap as an error. */
    promise.catch(() => {});
    return promise;
  }

  private async request(
    channel: number,
    opcode: number,
    payload: Uint8Array,
  ): Promise<{ op: number; body: Uint8Array }> {
    const replyPromise = this.waitForReply(channel);
    await this.send(channel, opcode, payload);
    return replyPromise;
  }
}
