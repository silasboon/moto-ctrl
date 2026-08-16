// Reference MOTO-CTRL protocol client over the simulator's WebSocket
// transport. Mirrors docs/PROTOCOL.md and firmware/components/core (mc_protocol.h)
// and doubles as a worked example for third-party clients. The app
// implements the same protocol in TypeScript behind its Transport interface.

import nacl from 'tweetnacl';
// Node only ships a global WebSocket client since v22 (stabilized from
// undici); CI pins node-version: 20 (.github/workflows/firmware.yml), which
// has none. Importing directly avoids depending on whichever Node happens
// to be running this — same fix as app/jest.setup.js for the app's own
// transport tests, applied here since this is plain Node, not Jest.
import WebSocket from 'ws';

// Channels (WebSocket message first byte == BLE GATT characteristic).
export const CH = { STATUS: 0, AUTH: 1, COMMAND: 2, CONFIG: 3, OTA: 4 };

// Opcodes (see mc_protocol.h).
export const OP = {
  STATUS_GET: 0x01, STATUS: 0x81,
  AUTH_BEGIN: 0x01, AUTH_CHALLENGE: 0x81,
  AUTH_RESPONSE: 0x02, AUTH_RESULT: 0x82,
  ENROLL: 0x10, ENROLL_RESULT: 0x90,
  KEY_LIST: 0x11, KEY_LIST_RESULT: 0x91,
  KEY_REVOKE: 0x12, KEY_REVOKE_RESULT: 0x92,
  SET_OUTPUT: 0x01, COMMAND_RESULT: 0x81,
  CONFIG_READ: 0x01, CONFIG_CHUNK: 0x81,
  CONFIG_WRITE_BEGIN: 0x02, CONFIG_WRITE_CHUNK: 0x03,
  CONFIG_WRITE_COMMIT: 0x04, CONFIG_WRITE_RESULT: 0x82,
  OTA_RESULT: 0x8f,
  // OTA (begin/chunk/commit/abort/reboot/status, all on the OTA
  // channel) and the event log (COMMAND channel).
  OTA_BEGIN: 0x01, OTA_CHUNK: 0x02, OTA_COMMIT: 0x03, OTA_ABORT: 0x04,
  OTA_REBOOT: 0x05, OTA_STATUS: 0x06, OTA_STATUS_RESULT: 0x86,
  EVENT_LOG_GET: 0x11, EVENT_LOG_CHUNK: 0x91,
  // Lock/immobilizer ops, all on the COMMAND channel.
  LOCK: 0x02, UNLOCK: 0x03,
  LOCK_GET_CONFIG: 0x04, LOCK_CONFIG: 0x84,
  LOCK_SET_CONFIG: 0x05,
  CHEATCODE_SET: 0x06, CHEATCODE_CLEAR: 0x07,
  CHEATCODE_TEST: 0x08, CHEATCODE_TEST_RESULT: 0x88,
  TRANSFER_OWNERSHIP: 0x09,
  // Diagnostics ops, all on the COMMAND channel.
  DIAG_GET: 0x0a, DIAG_RESULT: 0x8a,
  DIAG_GET_CONFIG: 0x0b, DIAG_CONFIG: 0x8b,
  DIAG_SET_CONFIG: 0x0c,
  DIAG_GET_CALIB: 0x0d, DIAG_CALIB: 0x8d,
  DIAG_SET_CALIB: 0x0e,
  DIAG_LEARN: 0x0f,
  // Flashers/PWM. Plain turn-signal control stays on SET_OUTPUT
  // above (mutual exclusion + auto-cancel are embedded device-side); this
  // is the only new opcode.
  HAZARD_PRESS: 0x10,
};

// mc_diag_fault_t (docs/PROTOCOL.md §12).
export const DIAG_FAULT = { NONE: 0, OPEN_LOAD: 1, OVERCURRENT: 2 };

export const RESULT = {
  OK: 0, UNAUTHENTICATED: 1, BAD_REQUEST: 2, REJECTED: 3,
  ENROLL_DENIED: 4, KEYSTORE_FULL: 5, NOT_FOUND: 6, NOT_IMPLEMENTED: 7, INTERNAL: 8,
};

// mc_lock_state_t (docs/PROTOCOL.md §5 status byte 3).
export const LOCK_STATE = { UNKNOWN: 0, PARKED: 1, LOCKED: 2, UNLOCKED: 3 };

// mc_ota_state_t (docs/PROTOCOL.md §10.1).
export const OTA_STATE = { IDLE: 0, RECEIVING: 1, COMMITTED: 2, ERROR: 3 };

// mc_event_type_t (docs/PROTOCOL.md §15).
export const EVENT_TYPE = {
  LOCK_ENGAGED: 1, LOCK_RELEASED: 2, KEY_ENROLLED: 3, KEY_REVOKED: 4,
  OWNERSHIP_TRANSFERRED: 5, FACTORY_RESET: 6, CHEATCODE_LOCKOUT: 7,
  OTA_BEGIN: 8, OTA_SUCCESS: 9, OTA_FAILURE: 10,
  LV_CUTOFF_ENTER: 11, LV_CUTOFF_EXIT: 12,
};

// mc_protocol.h's MC_LOCK_METHOD_* bits for lockSetConfig()'s methodsMask.
export const LOCK_METHOD = { PHONE: 1 << 0, IGNITION_SWITCH: 1 << 1 };

const AUTH_CONTEXT = new TextEncoder().encode('moto-ctrl-auth-v1');

// The exact message a client signs: the domain-separation context bytes
// followed by the 32-byte challenge nonce.
export function buildAuthMessage(nonce) {
  const msg = new Uint8Array(AUTH_CONTEXT.length + nonce.length);
  msg.set(AUTH_CONTEXT, 0);
  msg.set(nonce, AUTH_CONTEXT.length);
  return msg;
}

export function generateKeypair() {
  return nacl.sign.keyPair(); // { publicKey: 32, secretKey: 64 }
}

// The sim's fixed TEST OTA release keypair (firmware/sim/src/
// main.c's SIM_OTA_TEST_PUBKEY / secret) -- lets this client sign OTA
// images the sim's compiled-in public key will accept, without ever
// touching the real maintainer release key (mc_ota_release_key.c,
// real-target only, never used by the sim).
export const SIM_OTA_TEST_SECRET_KEY = new Uint8Array([
  0x29, 0xe2, 0x3d, 0xf5, 0x86, 0x10, 0x5b, 0x5c,
  0x6d, 0x32, 0x03, 0xc9, 0x01, 0x58, 0x42, 0x1a,
  0x10, 0xff, 0xcd, 0x94, 0x84, 0xa3, 0xa0, 0x66,
  0xf6, 0xa4, 0x9f, 0x67, 0x9e, 0x7e, 0x42, 0x81,
  0x53, 0x7c, 0xde, 0xa3, 0xcc, 0x7e, 0xe4, 0x52,
  0xda, 0xd1, 0xc2, 0x36, 0x66, 0x97, 0x0d, 0x73,
  0x0f, 0x9c, 0x9e, 0xfe, 0x8e, 0xee, 0xbf, 0x39,
  0x43, 0x76, 0x5e, 0xe0, 0xf2, 0x07, 0xd4, 0x73,
]);

export class MotoClient {
  constructor(url) {
    this.url = url;
    this.listeners = [];
  }

  connect() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.binaryType = 'arraybuffer';
      this.ws.onopen = () => resolve();
      this.ws.onerror = (e) => reject(new Error('ws error: ' + (e.message || 'unknown')));
      this.ws.onmessage = (ev) => {
        const b = new Uint8Array(ev.data);
        const frame = { ch: b[0], op: b[1], payload: b.slice(2) };
        this.listeners = this.listeners.filter((fn) => !fn(frame));
      };
    });
  }

  close() {
    if (this.ws) this.ws.close();
  }

  _send(ch, bytes) {
    const msg = new Uint8Array(1 + bytes.length);
    msg[0] = ch;
    msg.set(bytes, 1);
    this.ws.send(msg);
  }

  // Resolves with the first inbound frame matching (ch, op).
  _await(ch, op, timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.listeners = this.listeners.filter((f) => f !== listener);
        reject(new Error(`timeout waiting for ch=${ch} op=0x${op.toString(16)}`));
      }, timeoutMs);
      const listener = (frame) => {
        if (frame.ch === ch && frame.op === op) {
          clearTimeout(timer);
          resolve(frame);
          return true; // remove
        }
        return false;
      };
      this.listeners.push(listener);
    });
  }

  async getStatus() {
    this._send(CH.STATUS, Uint8Array.of(OP.STATUS_GET));
    const f = await this._await(CH.STATUS, OP.STATUS);
    const p = f.payload;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    return {
      fw: `${p[0]}.${p[1]}.${p[2]}`,
      lockState: p[3],
      uptimeMs: dv.getUint32(4, true),
      batteryMv: dv.getUint16(8, true),
      outputStateMask: dv.getUint16(10, true),
      outputFaultMask: dv.getUint16(12, true),
      rssiDbm: dv.getInt8(14),
      cheatcodeBackoff: (p[15] & 0x01) !== 0,
      lvCutoffActive: (p[15] & 0x02) !== 0,
    };
  }

  async enroll(publicKey, label = '') {
    const lbl = new TextEncoder().encode(label);
    const body = new Uint8Array(1 + publicKey.length + lbl.length);
    body[0] = OP.ENROLL;
    body.set(publicKey, 1);
    body.set(lbl, 1 + publicKey.length);
    this._send(CH.AUTH, body);
    const f = await this._await(CH.AUTH, OP.ENROLL_RESULT);
    return { result: f.payload[0], slot: f.payload[1] };
  }

  async authenticate(secretKey) {
    this._send(CH.AUTH, Uint8Array.of(OP.AUTH_BEGIN));
    const chal = await this._await(CH.AUTH, OP.AUTH_CHALLENGE);
    const nonce = chal.payload; // 32 bytes
    const sig = nacl.sign.detached(buildAuthMessage(nonce), secretKey);
    const body = new Uint8Array(1 + sig.length);
    body[0] = OP.AUTH_RESPONSE;
    body.set(sig, 1);
    this._send(CH.AUTH, body);
    const res = await this._await(CH.AUTH, OP.AUTH_RESULT);
    return { result: res.payload[0], slot: res.payload[1] };
  }

  // Sends a raw (possibly wrong) signature, for negative testing.
  async authenticateWithSignature(sig) {
    this._send(CH.AUTH, Uint8Array.of(OP.AUTH_BEGIN));
    await this._await(CH.AUTH, OP.AUTH_CHALLENGE);
    const body = new Uint8Array(1 + sig.length);
    body[0] = OP.AUTH_RESPONSE;
    body.set(sig, 1);
    this._send(CH.AUTH, body);
    const res = await this._await(CH.AUTH, OP.AUTH_RESULT);
    return { result: res.payload[0], slot: res.payload[1] };
  }

  async setOutput(channel, on) {
    this._send(CH.COMMAND, Uint8Array.of(OP.SET_OUTPUT, channel, on ? 1 : 0));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async keyList() {
    this._send(CH.AUTH, Uint8Array.of(OP.KEY_LIST));
    const f = await this._await(CH.AUTH, OP.KEY_LIST_RESULT);
    const p = f.payload;
    const count = p[0];
    const keys = [];
    let pos = 1;
    for (let i = 0; i < count; i++) {
      const slot = p[pos++];
      const len = p[pos++];
      const label = new TextDecoder().decode(p.slice(pos, pos + len));
      pos += len;
      keys.push({ slot, label });
    }
    return keys;
  }

  async revoke(slot) {
    this._send(CH.AUTH, Uint8Array.of(OP.KEY_REVOKE, slot));
    const f = await this._await(CH.AUTH, OP.KEY_REVOKE_RESULT);
    return { result: f.payload[0], slot: f.payload[1] };
  }

  // Collects the chunk stream from a CONFIG_READ into the full JSON string.
  configRead(timeoutMs = 3000) {
    return new Promise((resolve, reject) => {
      let total = null;
      let buf = null;
      let got = 0;
      const timer = setTimeout(() => {
        this.listeners = this.listeners.filter((f) => f !== listener);
        reject(new Error('timeout reading config'));
      }, timeoutMs);
      const listener = (frame) => {
        if (frame.ch !== CH.CONFIG || frame.op !== OP.CONFIG_CHUNK) return false;
        const p = frame.payload;
        const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
        const offset = dv.getUint16(0, true);
        total = dv.getUint16(2, true);
        const data = p.slice(4);
        if (buf === null) buf = new Uint8Array(total);
        buf.set(data, offset);
        got += data.length;
        if (got >= total) {
          clearTimeout(timer);
          resolve(new TextDecoder().decode(buf));
          return true;
        }
        return false;
      };
      this.listeners.push(listener);
      this._send(CH.CONFIG, Uint8Array.of(OP.CONFIG_READ));
    });
  }

  async configWrite(jsonString, chunkSize = 128) {
    const bytes = new TextEncoder().encode(jsonString);
    const begin = new Uint8Array(3);
    begin[0] = OP.CONFIG_WRITE_BEGIN;
    new DataView(begin.buffer).setUint16(1, bytes.length, true);
    this._send(CH.CONFIG, begin);

    for (let off = 0; off < bytes.length; off += chunkSize) {
      const n = Math.min(chunkSize, bytes.length - off);
      const chunk = new Uint8Array(3 + n);
      chunk[0] = OP.CONFIG_WRITE_CHUNK;
      new DataView(chunk.buffer).setUint16(1, off, true);
      chunk.set(bytes.slice(off, off + n), 3);
      this._send(CH.CONFIG, chunk);
    }

    this._send(CH.CONFIG, Uint8Array.of(OP.CONFIG_WRITE_COMMIT));
    const f = await this._await(CH.CONFIG, OP.CONFIG_WRITE_RESULT);
    return { result: f.payload[0] };
  }

  // --- Lock / immobilizer ---

  async lock() {
    this._send(CH.COMMAND, Uint8Array.of(OP.LOCK));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async unlock() {
    this._send(CH.COMMAND, Uint8Array.of(OP.UNLOCK));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async lockGetConfig() {
    this._send(CH.COMMAND, Uint8Array.of(OP.LOCK_GET_CONFIG));
    const f = await this._await(CH.COMMAND, OP.LOCK_CONFIG);
    const p = f.payload;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    return {
      immobilizerEnabled: p[0] !== 0,
      methodsMask: p[1],
      ignitionSwitchInput: p[2] === 0xff ? -1 : p[2],
      autoLockGraceMs: dv.getUint16(3, true),
      cheatcodeWindowMs: dv.getUint16(5, true),
      cheatcodeSet: p[7] !== 0,
      cheatcodeLen: p[8],
    };
  }

  // opts: { immobilizerEnabled, methodsMask, ignitionSwitchInput (-1 = none),
  //         autoLockGraceMs, cheatcodeWindowMs }
  async lockSetConfig(opts) {
    const body = new Uint8Array(8);
    body[0] = OP.LOCK_SET_CONFIG;
    body[1] = opts.immobilizerEnabled ? 1 : 0;
    body[2] = opts.methodsMask & 0xff;
    body[3] = opts.ignitionSwitchInput < 0 ? 0xff : opts.ignitionSwitchInput;
    const dv = new DataView(body.buffer);
    dv.setUint16(4, opts.autoLockGraceMs, true);
    dv.setUint16(6, opts.cheatcodeWindowMs, true);
    this._send(CH.COMMAND, body);
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async cheatcodeSet(buttons) {
    const body = new Uint8Array(2 + buttons.length);
    body[0] = OP.CHEATCODE_SET;
    body[1] = buttons.length;
    body.set(buttons, 2);
    this._send(CH.COMMAND, body);
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async cheatcodeClear() {
    this._send(CH.COMMAND, Uint8Array.of(OP.CHEATCODE_CLEAR));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async cheatcodeTest(buttons) {
    const body = new Uint8Array(2 + buttons.length);
    body[0] = OP.CHEATCODE_TEST;
    body[1] = buttons.length;
    body.set(buttons, 2);
    this._send(CH.COMMAND, body);
    const f = await this._await(CH.COMMAND, OP.CHEATCODE_TEST_RESULT);
    return { result: f.payload[0], match: f.payload[1] !== 0 };
  }

  async transferOwnership() {
    this._send(CH.COMMAND, Uint8Array.of(OP.TRANSFER_OWNERSHIP));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  // --- Diagnostics ---

  async getDiagnostics() {
    this._send(CH.COMMAND, Uint8Array.of(OP.DIAG_GET));
    const f = await this._await(CH.COMMAND, OP.DIAG_RESULT);
    const p = f.payload; // [result:1] then 12*current_ma:u16le, then 12*fault:1
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    const result = p[0];
    const channels = [];
    let pos = 1;
    const currents = [];
    for (let i = 0; i < 12; i++) {
      currents.push(dv.getUint16(pos, true));
      pos += 2;
    }
    for (let i = 0; i < 12; i++) {
      channels.push({ currentMa: currents[i], fault: p[pos] });
      pos += 1;
    }
    return { result, channels };
  }

  async diagGetConfig() {
    this._send(CH.COMMAND, Uint8Array.of(OP.DIAG_GET_CONFIG));
    const f = await this._await(CH.COMMAND, OP.DIAG_CONFIG);
    const p = f.payload;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    const result = p[0];
    const channels = [];
    let pos = 1;
    for (let i = 0; i < 12; i++) {
      const openLoadMa = dv.getUint16(pos, true); pos += 2;
      const overcurrentMa = dv.getUint16(pos, true); pos += 2;
      channels.push({ openLoadMa, overcurrentMa });
    }
    const lvCutoffMv = dv.getUint16(pos, true); pos += 2;
    const lvCutoffHysteresisMv = dv.getUint16(pos, true); pos += 2;
    const engineRunMv = dv.getUint16(pos, true); pos += 2;
    const engineRunHysteresisMv = dv.getUint16(pos, true); pos += 2;
    return { result, channels, lvCutoffMv, lvCutoffHysteresisMv, engineRunMv, engineRunHysteresisMv };
  }

  // cfg: { channels: [{openLoadMa, overcurrentMa}, ...12], lvCutoffMv,
  //        lvCutoffHysteresisMv, engineRunMv, engineRunHysteresisMv }
  async diagSetConfig(cfg) {
    const body = new Uint8Array(1 + 12 * 4 + 8);
    body[0] = OP.DIAG_SET_CONFIG;
    const dv = new DataView(body.buffer);
    let pos = 1;
    for (let i = 0; i < 12; i++) {
      dv.setUint16(pos, cfg.channels[i].openLoadMa, true); pos += 2;
      dv.setUint16(pos, cfg.channels[i].overcurrentMa, true); pos += 2;
    }
    dv.setUint16(pos, cfg.lvCutoffMv, true); pos += 2;
    dv.setUint16(pos, cfg.lvCutoffHysteresisMv, true); pos += 2;
    dv.setUint16(pos, cfg.engineRunMv, true); pos += 2;
    dv.setUint16(pos, cfg.engineRunHysteresisMv, true); pos += 2;
    this._send(CH.COMMAND, body);
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  async diagGetCalib() {
    this._send(CH.COMMAND, Uint8Array.of(OP.DIAG_GET_CALIB));
    const f = await this._await(CH.COMMAND, OP.DIAG_CALIB);
    const p = f.payload;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    return {
      result: p[0],
      isGain: dv.getFloat32(1, true),
      isOffsetMv: dv.getInt16(5, true),
      kilis: dv.getFloat32(7, true),
      vbatGain: dv.getFloat32(11, true),
      vbatOffsetMv: dv.getInt16(15, true),
    };
  }

  // calib: { isGain, isOffsetMv, kilis, vbatGain, vbatOffsetMv }
  async diagSetCalib(calib) {
    const body = new Uint8Array(1 + 16);
    body[0] = OP.DIAG_SET_CALIB;
    const dv = new DataView(body.buffer);
    dv.setFloat32(1, calib.isGain, true);
    dv.setInt16(5, calib.isOffsetMv, true);
    dv.setFloat32(7, calib.kilis, true);
    dv.setFloat32(11, calib.vbatGain, true);
    dv.setInt16(15, calib.vbatOffsetMv, true);
    this._send(CH.COMMAND, body);
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  // channel: 0-11, or 0xff for "every currently energized channel".
  async diagLearn(channel) {
    this._send(CH.COMMAND, Uint8Array.of(OP.DIAG_LEARN, channel));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  // --- Flashers / PWM ---

  // Toggles hazards: both TURN_L and TURN_R together, no mutual exclusion,
  // no auto-cancel timer. REJECTED if neither channel is configured.
  async hazardPress() {
    this._send(CH.COMMAND, Uint8Array.of(OP.HAZARD_PRESS));
    const f = await this._await(CH.COMMAND, OP.COMMAND_RESULT);
    return { reqOpcode: f.payload[0], result: f.payload[1] };
  }

  // --- OTA ---

  async otaBegin(imageSize, sha512, signature) {
    const body = new Uint8Array(1 + 4 + 64 + 64);
    body[0] = OP.OTA_BEGIN;
    new DataView(body.buffer).setUint32(1, imageSize, true);
    body.set(sha512, 5);
    body.set(signature, 5 + 64);
    this._send(CH.OTA, body);
    const f = await this._await(CH.OTA, OP.OTA_RESULT);
    return { result: f.payload[0] };
  }

  async otaChunk(offset, data) {
    const body = new Uint8Array(1 + 4 + data.length);
    body[0] = OP.OTA_CHUNK;
    new DataView(body.buffer).setUint32(1, offset, true);
    body.set(data, 5);
    this._send(CH.OTA, body);
    const f = await this._await(CH.OTA, OP.OTA_RESULT);
    return { result: f.payload[0] };
  }

  async otaCommit() {
    this._send(CH.OTA, Uint8Array.of(OP.OTA_COMMIT));
    const f = await this._await(CH.OTA, OP.OTA_RESULT);
    return { result: f.payload[0] };
  }

  async otaAbort() {
    this._send(CH.OTA, Uint8Array.of(OP.OTA_ABORT));
    const f = await this._await(CH.OTA, OP.OTA_RESULT);
    return { result: f.payload[0] };
  }

  async otaReboot() {
    this._send(CH.OTA, Uint8Array.of(OP.OTA_REBOOT));
    const f = await this._await(CH.OTA, OP.OTA_RESULT);
    return { result: f.payload[0] };
  }

  async otaStatus() {
    this._send(CH.OTA, Uint8Array.of(OP.OTA_STATUS));
    const f = await this._await(CH.OTA, OP.OTA_STATUS_RESULT);
    const p = f.payload;
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    return {
      result: p[0],
      state: p[1],
      bytesReceived: dv.getUint32(2, true),
      imageSize: dv.getUint32(6, true),
    };
  }

  // High-level: hashes + signs `image` with `secretKey` (use
  // SIM_OTA_TEST_SECRET_KEY against this sim) and drives the full begin /
  // chunk* / commit sequence. Stops early (without committing) if begin or
  // any chunk is rejected.
  async otaTransfer(image, secretKey, chunkSize = 512) {
    const digest = nacl.hash(image); // SHA-512, 64 bytes
    const signature = nacl.sign.detached(digest, secretKey);
    const beginRes = await this.otaBegin(image.length, digest, signature);
    if (beginRes.result !== RESULT.OK) {
      return { stage: 'begin', ...beginRes };
    }
    for (let off = 0; off < image.length; off += chunkSize) {
      const n = Math.min(chunkSize, image.length - off);
      const r = await this.otaChunk(off, image.slice(off, off + n));
      if (r.result !== RESULT.OK) {
        return { stage: 'chunk', offset: off, ...r };
      }
    }
    const commitRes = await this.otaCommit();
    return { stage: 'commit', ...commitRes };
  }

  // --- Event log ---

  // Collects the chunk stream from an EVENT_LOG_GET into a flat array of
  // { seq, uptimeMs, type, arg0, arg1 }, oldest-first.
  eventLogGet(sinceSeq = 0, timeoutMs = 5000) {
    return new Promise((resolve, reject) => {
      let total = null;
      const records = [];
      const timer = setTimeout(() => {
        this.listeners = this.listeners.filter((f) => f !== listener);
        reject(new Error('timeout reading event log'));
      }, timeoutMs);
      const listener = (frame) => {
        if (frame.ch !== CH.COMMAND || frame.op !== OP.EVENT_LOG_CHUNK) return false;
        const p = frame.payload;
        const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
        total = dv.getUint16(2, true);
        const count = p[4];
        let pos = 5;
        for (let i = 0; i < count; i++) {
          const seq = dv.getUint32(pos, true); pos += 4;
          const uptimeMs = dv.getUint32(pos, true); pos += 4;
          const type = p[pos++];
          const arg0 = p[pos++];
          const arg1 = p[pos++];
          pos += 1; // reserved
          records.push({ seq, uptimeMs, type, arg0, arg1 });
        }
        if (records.length >= total) {
          clearTimeout(timer);
          resolve(records);
          return true;
        }
        return false;
      };
      this.listeners.push(listener);
      const body = new Uint8Array(5);
      body[0] = OP.EVENT_LOG_GET;
      new DataView(body.buffer).setUint32(1, sinceSeq, true);
      this._send(CH.COMMAND, body);
    });
  }
}
