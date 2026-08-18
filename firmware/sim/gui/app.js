'use strict';

/*
 * MOTO-CTRL sim debug console.
 *
 * Talks the real wire protocol (docs/PROTOCOL.md, mc_protocol.h) over the
 * WebSocket transport, plus the sim-only debug/fault-injection channel
 * (firmware/sim/src/sim_protocol.h) that only exists on this simulator —
 * never on real hardware. Frame layout on both: [channel byte][opcode byte]
 * [payload...]. Ed25519 (TweetNaCl) plays the role of a paired phone's key.
 */

// ---- protocol constants (mirrors mc_protocol.h / sim_protocol.h) ----

const MC_CH = { STATUS: 0, AUTH: 1, COMMAND: 2, CONFIG: 3, OTA: 4 };
const SIM_CH_DEBUG = 0x7f;

const MC_OP = {
    STATUS_GET: 0x01, STATUS: 0x81,
    AUTH_BEGIN: 0x01, AUTH_CHALLENGE: 0x81, AUTH_RESPONSE: 0x02, AUTH_RESULT: 0x82,
    ENROLL: 0x10, ENROLL_RESULT: 0x90, KEY_LIST: 0x11, KEY_LIST_RESULT: 0x91,
    KEY_REVOKE: 0x12, KEY_REVOKE_RESULT: 0x92,
    SET_OUTPUT: 0x01, COMMAND_RESULT: 0x81,
    CONFIG_READ: 0x01, CONFIG_CHUNK: 0x81, CONFIG_WRITE_BEGIN: 0x02,
    CONFIG_WRITE_CHUNK: 0x03, CONFIG_WRITE_COMMIT: 0x04, CONFIG_WRITE_RESULT: 0x82,
    // OTA (all on the OTA channel) and event log (COMMAND channel).
    OTA_BEGIN: 0x01, OTA_CHUNK: 0x02, OTA_COMMIT: 0x03, OTA_ABORT: 0x04,
    OTA_REBOOT: 0x05, OTA_STATUS: 0x06, OTA_STATUS_RESULT: 0x86, OTA_RESULT: 0x8f,
    EVENT_LOG_GET: 0x11, EVENT_LOG_CHUNK: 0x91,
    // Lock/immobilizer, all on the COMMAND channel.
    LOCK: 0x02, UNLOCK: 0x03,
    LOCK_GET_CONFIG: 0x04, LOCK_CONFIG: 0x84,
    LOCK_SET_CONFIG: 0x05,
    CHEATCODE_SET: 0x06, CHEATCODE_CLEAR: 0x07,
    CHEATCODE_TEST: 0x08, CHEATCODE_TEST_RESULT: 0x88,
    TRANSFER_OWNERSHIP: 0x09,
    // Diagnostics, all on the COMMAND channel.
    DIAG_GET: 0x0a, DIAG_RESULT: 0x8a,
    DIAG_GET_CONFIG: 0x0b, DIAG_CONFIG: 0x8b,
    DIAG_SET_CONFIG: 0x0c,
    DIAG_GET_CALIB: 0x0d, DIAG_CALIB: 0x8d,
    DIAG_SET_CALIB: 0x0e,
    DIAG_LEARN: 0x0f,
    // Flashers/PWM. Plain turn-signal control still just uses
    // SET_OUTPUT above (mutual exclusion + auto-cancel are embedded
    // device-side, in mc_output_set() itself) -- this is the only new
    // opcode. Mode (pwm/flash_turn/flash_brake), duty, auto-cancel/blink/
    // pulse timing, and the brake-switch input all ride the Config JSON
    // panel below, same as starter_interlock_input always has.
    HAZARD_PRESS: 0x10,
};

const LOCK_METHOD = { PHONE: 1 << 0, IGNITION_SWITCH: 1 << 1 };

const SIM_OP = {
    SET_BATTERY_MV: 0x01, SET_CHANNEL_FAULT: 0x02, SET_ENGINE_RUNNING: 0x03,
    SET_INTERLOCK: 0x04, BUTTON_STATE: 0x05, FORCE_DISCONNECT: 0x06,
    FORCE_REBOOT: 0x07, FORCE_NVS_CORRUPT: 0x08, GET_LOG: 0x09, GET_STATE: 0x0a,
    RESET_FAULTS: 0x0b, ACK: 0x81, LOG_ENTRY: 0x90, STATE: 0x91,
};

const RESULT = {
    0: 'OK', 1: 'UNAUTHENTICATED', 2: 'BAD_REQUEST', 3: 'REJECTED',
    4: 'ENROLL_DENIED', 5: 'KEYSTORE_FULL', 6: 'NOT_FOUND', 7: 'NOT_IMPLEMENTED', 8: 'INTERNAL',
};

const LOCK_STATE = { 0: 'UNKNOWN', 1: 'PARKED', 2: 'LOCKED', 3: 'UNLOCKED' };
const OTA_STATE = { 0: 'IDLE', 1: 'RECEIVING', 2: 'COMMITTED', 3: 'ERROR' };
// mc_event_type_t (docs/PROTOCOL.md §15).
const EVENT_TYPE_NAMES = {
    1: 'LOCK_ENGAGED', 2: 'LOCK_RELEASED', 3: 'KEY_ENROLLED', 4: 'KEY_REVOKED',
    5: 'OWNERSHIP_TRANSFERRED', 6: 'FACTORY_RESET', 7: 'CHEATCODE_LOCKOUT',
    8: 'OTA_BEGIN', 9: 'OTA_SUCCESS', 10: 'OTA_FAILURE',
    11: 'LV_CUTOFF_ENTER', 12: 'LV_CUTOFF_EXIT',
};
const FAULT_NAMES = ['none', 'open_load', 'overcurrent', 'short']; // index 3 (short) is sim-legacy; mc_diag never reports it
const OUTPUT_COUNT = 12;
const INPUT_COUNT = 8;
const AUTH_CONTEXT = new TextEncoder().encode('moto-ctrl-auth-v1');
const CONFIG_JSON_MAX = 4096;
const CONFIG_CHUNK = 128;
const OTA_CHUNK_BYTES = 4096;

// Fixed TEST OTA release keypair -- the sim's compiled-in public
// key (SIM_OTA_TEST_PUBKEY, firmware/sim/src/main.c) accepts images signed
// by this secret key, and only this one. Identical bytes to
// firmware/sim/itest/moto-client.mjs's SIM_OTA_TEST_SECRET_KEY. Never the
// real maintainer release key (mc_ota_release_key.c, real-target only,
// never used by the sim) -- same "TweetNaCl plays the phone" doctrine as
// the phone keypairs above, applied to OTA instead of auth.
const SIM_OTA_TEST_SECRET_KEY = new Uint8Array([
    0x29, 0xe2, 0x3d, 0xf5, 0x86, 0x10, 0x5b, 0x5c,
    0x6d, 0x32, 0x03, 0xc9, 0x01, 0x58, 0x42, 0x1a,
    0x10, 0xff, 0xcd, 0x94, 0x84, 0xa3, 0xa0, 0x66,
    0xf6, 0xa4, 0x9f, 0x67, 0x9e, 0x7e, 0x42, 0x81,
    0x53, 0x7c, 0xde, 0xa3, 0xcc, 0x7e, 0xe4, 0x52,
    0xda, 0xd1, 0xc2, 0x36, 0x66, 0x97, 0x0d, 0x73,
    0x0f, 0x9c, 0x9e, 0xfe, 0x8e, 0xee, 0xbf, 0x39,
    0x43, 0x76, 0x5e, 0xe0, 0xf2, 0x07, 0xd4, 0x73,
]);

function resultName(r) { return RESULT[r] || `?(${r})`; }

// ---- byte helpers ----

function b64encode(bytes) {
    let bin = '';
    for (const b of bytes) bin += String.fromCharCode(b);
    return btoa(bin);
}
function b64decode(str) {
    const bin = atob(str);
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
}
function u16le(v) { return new Uint8Array([v & 0xff, (v >> 8) & 0xff]); }
function u32le(v) { const b = new Uint8Array(4); new DataView(b.buffer).setUint32(0, v, true); return b; }
function readU16le(b, off) { return b[off] | (b[off + 1] << 8); }
function readU32le(b, off) { return (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) | (b[off + 3] << 24)) >>> 0; }
function concatBytes(...parts) {
    const len = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array(len);
    let o = 0;
    for (const p of parts) { out.set(p, o); o += p.length; }
    return out;
}

// ---- state ----

const state = {
    ws: null,
    connected: false,
    url: 'ws://127.0.0.1:8010',
    autoReconnect: true,
    reconnectTimer: null,

    status: null, // {fw, lockState, uptimeMs, batteryMv, outMask, faultMask, rssi, lvCutoffActive}
    outputOn: new Array(OUTPUT_COUNT).fill(false),
    channelSim: Array.from({ length: OUTPUT_COUNT }, () => ({ current: 0, fault: 0 })),
    buttonPressed: new Array(INPUT_COUNT).fill(false),
    diagConfig: null, // {channels:[{openLoadMa,overcurrentMa}], lvCutoffMv, lvCutoffHysteresisMv, engineRunMv, engineRunHysteresisMv}
    diagCalib: null,  // {isGain, isOffsetMv, kilis, vbatGain, vbatOffsetMv}

    phones: [],
    authedSlot: -1,
    pendingAuthPhone: null,

    configWriteOffset: 0,
    configWriteBytes: null,
    configChunks: {}, // offset -> Uint8Array, for CONFIG_READ reassembly

    recording: false,
    recordStart: 0,
    recordBuf: [],
};

function loadPhones() {
    try {
        const raw = localStorage.getItem('motoctrl_sim_phones');
        if (!raw) return;
        const arr = JSON.parse(raw);
        state.phones = arr.map((p) => ({
            label: p.label,
            publicKey: b64decode(p.publicKey),
            secretKey: b64decode(p.secretKey),
            slot: null,
        }));
    } catch (e) { /* corrupt localStorage: start fresh, this is a dev tool */ }
}
function savePhones() {
    const arr = state.phones.map((p) => ({
        label: p.label, publicKey: b64encode(p.publicKey), secretKey: b64encode(p.secretKey),
    }));
    localStorage.setItem('motoctrl_sim_phones', JSON.stringify(arr));
}

// ---- transport ----

function connect(url) {
    if (state.ws) return;
    state.url = url;
    const ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
        state.connected = true;
        state.ws = ws;
        setConnBadge(true);
        send(SIM_CH_DEBUG, SIM_OP.GET_LOG, new Uint8Array(0));
        send(SIM_CH_DEBUG, SIM_OP.GET_STATE, new Uint8Array(0));
        send(MC_CH.STATUS, MC_OP.STATUS_GET, new Uint8Array(0));
    };
    ws.onmessage = (ev) => onFrame(new Uint8Array(ev.data));
    ws.onclose = () => {
        state.connected = false;
        state.ws = null;
        state.authedSlot = -1;
        for (const p of state.phones) p.slot = null;
        setConnBadge(false);
        renderPhones();
        if (state.autoReconnect) {
            clearTimeout(state.reconnectTimer);
            state.reconnectTimer = setTimeout(() => connect(state.url), 1000);
        }
    };
    ws.onerror = () => {};
}

function disconnect() {
    state.autoReconnect = false;
    document.getElementById('chk-autoreconnect').checked = false;
    clearTimeout(state.reconnectTimer);
    if (state.ws) state.ws.close();
}

function send(ch, opcode, payload) {
    if (!state.ws || state.ws.readyState !== WebSocket.OPEN) return;
    state.ws.send(concatBytes(new Uint8Array([ch, opcode]), payload));
}

function setConnBadge(connected) {
    const el = document.getElementById('conn-badge');
    el.className = 'badge ' + (connected ? 'connected' : 'disconnected');
    el.lastChild.textContent = connected ? 'connected' : 'disconnected';
    document.getElementById('btn-connect').disabled = connected;
    document.getElementById('btn-disconnect').disabled = !connected;
}

// ---- frame dispatch ----

function onFrame(bytes) {
    if (bytes.length < 2) return;
    const ch = bytes[0], op = bytes[1], payload = bytes.subarray(2);
    if (ch === MC_CH.STATUS && op === MC_OP.STATUS) return onStatus(payload);
    if (ch === MC_CH.AUTH) return onAuthFrame(op, payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.COMMAND_RESULT) return onCommandResult(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.LOCK_CONFIG) return onLockConfig(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.CHEATCODE_TEST_RESULT) return onCheatcodeTestResult(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.DIAG_CONFIG) return onDiagConfig(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.DIAG_CALIB) return onDiagCalib(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.DIAG_RESULT) return onDiagResult(payload);
    if (ch === MC_CH.COMMAND && op === MC_OP.EVENT_LOG_CHUNK) return onEventLogChunk(payload);
    if (ch === MC_CH.CONFIG) return onConfigFrame(op, payload);
    if (ch === MC_CH.OTA) return onOtaFrame(op, payload);
    if (ch === SIM_CH_DEBUG) return onDebugFrame(op, payload);
}

// ---- STATUS ----

function onStatus(p) {
    state.status = {
        fw: `${p[0]}.${p[1]}.${p[2]}`,
        lockState: p[3],
        uptimeMs: readU32le(p, 4),
        batteryMv: readU16le(p, 8),
        outMask: readU16le(p, 10),
        faultMask: readU16le(p, 12),
        rssi: (p[14] << 24) >> 24,
        cheatcodeBackoff: !!(p[15] & 0x01),
        lvCutoffActive: !!(p[15] & 0x02),
    };
    for (let c = 0; c < OUTPUT_COUNT; c++) state.outputOn[c] = !!(state.status.outMask & (1 << c));
    renderStatus();
    updateChannelsDisplay();
}

// ---- AUTH ----

function onAuthFrame(op, p) {
    if (op === MC_OP.AUTH_CHALLENGE) return onAuthChallenge(p);
    if (op === MC_OP.AUTH_RESULT) return onAuthResult(p);
    if (op === MC_OP.ENROLL_RESULT) return onEnrollResult(p);
    if (op === MC_OP.KEY_LIST_RESULT) return onKeyListResult(p);
    if (op === MC_OP.KEY_REVOKE_RESULT) return onKeyRevokeResult(p);
}

function authBegin(phone) {
    state.pendingAuthPhone = phone;
    send(MC_CH.AUTH, MC_OP.AUTH_BEGIN, new Uint8Array(0));
}

function onAuthChallenge(nonce) {
    const phone = state.pendingAuthPhone;
    if (!phone) return;
    const msg = concatBytes(AUTH_CONTEXT, nonce);
    const sig = nacl.sign.detached(msg, phone.secretKey);
    send(MC_CH.AUTH, MC_OP.AUTH_RESPONSE, sig);
}

function onAuthResult(p) {
    const result = p[0], slot = p[1];
    const phone = state.pendingAuthPhone;
    state.pendingAuthPhone = null;
    if (result === 0) {
        state.authedSlot = slot;
        if (phone) phone.slot = slot;
        logLocal(`auth OK: ${phone ? phone.label : '?'} -> slot ${slot}`);
    } else {
        logLocal(`auth FAILED (${phone ? phone.label : '?'}): ${resultName(result)}`);
    }
    renderPhones();
}

function enroll(phone) {
    const label = new TextEncoder().encode(phone.label).slice(0, 23);
    send(MC_CH.AUTH, MC_OP.ENROLL, concatBytes(phone.publicKey, label));
}
function onEnrollResult(p) {
    const result = p[0], slot = p[1];
    logLocal(`enroll: ${resultName(result)}${result === 0 ? ' -> slot ' + slot : ''}`);
}

function keyList() { send(MC_CH.AUTH, MC_OP.KEY_LIST, new Uint8Array(0)); }
function onKeyListResult(p) {
    const count = p[0];
    logLocal(`key list: ${count} enrolled`);
}

function keyRevoke(slot) { send(MC_CH.AUTH, MC_OP.KEY_REVOKE, new Uint8Array([slot])); }
function onKeyRevokeResult(p) {
    const result = p[0], slot = p[1];
    logLocal(`revoke slot ${slot}: ${resultName(result)}`);
    if (result === 0) {
        for (const ph of state.phones) if (ph.slot === slot) ph.slot = null;
        renderPhones();
    }
}

// ---- COMMAND ----

function cmdSetOutput(channel, on) {
    send(MC_CH.COMMAND, MC_OP.SET_OUTPUT, new Uint8Array([channel, on ? 1 : 0]));
}
function onCommandResult(p) {
    const reqOp = p[0], result = p[1];
    if (result !== 0) logLocal(`command result: op=0x${reqOp.toString(16)} ${resultName(result)}`);
}

// ---- Flashers/PWM ----

// Toggles both TURN_L and TURN_R together (no mutual-exclusion cancellation,
// no auto-cancel timer armed) -- REJECTED if neither channel is configured
// with a function of turn_l/turn_r in the Config JSON panel below.
function cmdHazardPress() { send(MC_CH.COMMAND, MC_OP.HAZARD_PRESS, new Uint8Array(0)); }

// ---- Lock / immobilizer ----

function cmdLock() { send(MC_CH.COMMAND, MC_OP.LOCK, new Uint8Array(0)); }
function cmdUnlock() { send(MC_CH.COMMAND, MC_OP.UNLOCK, new Uint8Array(0)); }
function cmdLockGetConfig() { send(MC_CH.COMMAND, MC_OP.LOCK_GET_CONFIG, new Uint8Array(0)); }

function cmdLockSetConfig({ enabled, methodsMask, ignitionSwitchInput, graceMs, windowMs }) {
    const body = new Uint8Array(7);
    body[0] = enabled ? 1 : 0;
    body[1] = methodsMask & 0xff;
    body[2] = ignitionSwitchInput < 0 ? 0xff : ignitionSwitchInput;
    const dv = new DataView(body.buffer);
    dv.setUint16(3, graceMs, true);
    dv.setUint16(5, windowMs, true);
    send(MC_CH.COMMAND, MC_OP.LOCK_SET_CONFIG, body);
}

function cmdCheatcodeSet(buttons) {
    send(MC_CH.COMMAND, MC_OP.CHEATCODE_SET, concatBytes(new Uint8Array([buttons.length]), new Uint8Array(buttons)));
}
function cmdCheatcodeClear() { send(MC_CH.COMMAND, MC_OP.CHEATCODE_CLEAR, new Uint8Array(0)); }
function cmdCheatcodeTest(buttons) {
    send(MC_CH.COMMAND, MC_OP.CHEATCODE_TEST, concatBytes(new Uint8Array([buttons.length]), new Uint8Array(buttons)));
}
function cmdTransferOwnership() { send(MC_CH.COMMAND, MC_OP.TRANSFER_OWNERSHIP, new Uint8Array(0)); }

function parseCodeInput(text) {
    return text.split(',').map((s) => parseInt(s.trim(), 10)).filter((n) => Number.isInteger(n) && n >= 0 && n <= 7);
}

function onLockConfig(p) {
    const cfg = {
        enabled: !!p[0],
        methodsMask: p[1],
        ignitionSwitchInput: p[2] === 0xff ? -1 : p[2],
        graceMs: readU16le(p, 3),
        windowMs: readU16le(p, 5),
        cheatcodeSet: !!p[7],
        cheatcodeLen: p[8],
    };
    document.getElementById('lk-enabled').checked = cfg.enabled;
    document.getElementById('lk-method-phone').checked = !!(cfg.methodsMask & LOCK_METHOD.PHONE);
    document.getElementById('lk-method-ignswitch').checked = !!(cfg.methodsMask & LOCK_METHOD.IGNITION_SWITCH);
    document.getElementById('lk-ignswitch-input').value = String(cfg.ignitionSwitchInput);
    document.getElementById('lk-grace-ms').value = cfg.graceMs;
    document.getElementById('lk-window-ms').value = cfg.windowMs;
    document.getElementById('lk-config-summary').textContent =
        `cheat-code: ${cfg.cheatcodeSet ? `set (${cfg.cheatcodeLen} presses)` : 'not set'}`;
    logLocal(`lock config: enabled=${cfg.enabled} methods=0x${cfg.methodsMask.toString(16)} ` +
             `ignSwitch=${cfg.ignitionSwitchInput} grace=${cfg.graceMs}ms window=${cfg.windowMs}ms`);
}

function onCheatcodeTestResult(p) {
    const el = document.getElementById('lk-test-result');
    const result = p[0], match = !!p[1];
    if (result !== 0) {
        el.className = 'fail';
        el.textContent = resultName(result);
        return;
    }
    el.className = match ? 'pass' : 'neutral';
    el.textContent = match ? 'MATCH' : 'no match';
}

// ---- Diagnostics ----

function cmdDiagGet() { send(MC_CH.COMMAND, MC_OP.DIAG_GET, new Uint8Array(0)); }
function cmdDiagGetConfig() { send(MC_CH.COMMAND, MC_OP.DIAG_GET_CONFIG, new Uint8Array(0)); }

function cmdDiagSetConfig(cfg) {
    const body = new Uint8Array(1 + OUTPUT_COUNT * 4 + 8 + 1);
    const dv = new DataView(body.buffer);
    let pos = 0;
    body[pos++] = MC_OP.DIAG_SET_CONFIG;
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        dv.setUint16(pos, cfg.channels[c].openLoadMa, true); pos += 2;
        dv.setUint16(pos, cfg.channels[c].overcurrentMa, true); pos += 2;
    }
    dv.setUint16(pos, cfg.lvCutoffMv, true); pos += 2;
    dv.setUint16(pos, cfg.lvCutoffHysteresisMv, true); pos += 2;
    dv.setUint16(pos, cfg.engineRunMv, true); pos += 2;
    dv.setUint16(pos, cfg.engineRunHysteresisMv, true); pos += 2;
    dv.setUint8(pos, cfg.engineRunVoltageDetectionEnabled ? 1 : 0); pos += 1;
    send(MC_CH.COMMAND, MC_OP.DIAG_SET_CONFIG, body.subarray(1));
}

function cmdDiagGetCalib() { send(MC_CH.COMMAND, MC_OP.DIAG_GET_CALIB, new Uint8Array(0)); }

function cmdDiagSetCalib(calib) {
    const body = new Uint8Array(16);
    const dv = new DataView(body.buffer);
    dv.setFloat32(0, calib.isGain, true);
    dv.setInt16(4, calib.isOffsetMv, true);
    dv.setFloat32(6, calib.kilis, true);
    dv.setFloat32(10, calib.vbatGain, true);
    dv.setInt16(14, calib.vbatOffsetMv, true);
    send(MC_CH.COMMAND, MC_OP.DIAG_SET_CALIB, body);
}

function cmdDiagLearn(channel) { send(MC_CH.COMMAND, MC_OP.DIAG_LEARN, new Uint8Array([channel])); }

function onDiagResult(p) {
    // [result:1] then OUTPUT_COUNT*current_ma:u16le, then OUTPUT_COUNT*fault:1
    if (p[0] !== 0) { logLocal(`diag get: ${resultName(p[0])}`); return; }
    let pos = 1;
    const currents = [];
    for (let c = 0; c < OUTPUT_COUNT; c++) { currents.push(readU16le(p, pos)); pos += 2; }
    for (let c = 0; c < OUTPUT_COUNT; c++) { state.channelSim[c] = { current: currents[c], fault: p[pos] }; pos += 1; }
    updateChannelsDisplay();
}

function onDiagConfig(p) {
    if (p[0] !== 0) { logLocal(`diag get config: ${resultName(p[0])}`); return; }
    let pos = 1;
    const channels = [];
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        const openLoadMa = readU16le(p, pos); pos += 2;
        const overcurrentMa = readU16le(p, pos); pos += 2;
        channels.push({ openLoadMa, overcurrentMa });
    }
    const lvCutoffMv = readU16le(p, pos); pos += 2;
    const lvCutoffHysteresisMv = readU16le(p, pos); pos += 2;
    const engineRunMv = readU16le(p, pos); pos += 2;
    const engineRunHysteresisMv = readU16le(p, pos); pos += 2;
    const engineRunVoltageDetectionEnabled = p[pos] !== 0; pos += 1;
    state.diagConfig = { channels, lvCutoffMv, lvCutoffHysteresisMv, engineRunMv, engineRunHysteresisMv, engineRunVoltageDetectionEnabled };
    renderDiagConfig();
    logLocal('diag config refreshed');
}

function onDiagCalib(p) {
    if (p[0] !== 0) { logLocal(`diag get calib: ${resultName(p[0])}`); return; }
    const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
    state.diagCalib = {
        isGain: dv.getFloat32(1, true),
        isOffsetMv: dv.getInt16(5, true),
        kilis: dv.getFloat32(7, true),
        vbatGain: dv.getFloat32(11, true),
        vbatOffsetMv: dv.getInt16(15, true),
    };
    renderDiagCalib();
    logLocal('diag calibration refreshed');
}

function readDiagConfigFromDom() {
    const channels = [];
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        channels.push({
            openLoadMa: parseInt(document.getElementById(`dch-${c}-open`).value, 10) || 0,
            overcurrentMa: parseInt(document.getElementById(`dch-${c}-over`).value, 10) || 0,
        });
    }
    return {
        channels,
        lvCutoffMv: parseInt(document.getElementById('diag-lv-cutoff-mv').value, 10) || 0,
        lvCutoffHysteresisMv: parseInt(document.getElementById('diag-lv-cutoff-hyst-mv').value, 10) || 0,
        engineRunMv: parseInt(document.getElementById('diag-engine-run-mv').value, 10) || 0,
        engineRunHysteresisMv: parseInt(document.getElementById('diag-engine-run-hyst-mv').value, 10) || 0,
        engineRunVoltageDetectionEnabled: document.getElementById('diag-engine-run-detect-enabled').checked,
    };
}

function readDiagCalibFromDom() {
    return {
        isGain: parseFloat(document.getElementById('diag-calib-is-gain').value) || 0,
        isOffsetMv: parseInt(document.getElementById('diag-calib-is-offset').value, 10) || 0,
        kilis: parseFloat(document.getElementById('diag-calib-kilis').value) || 0,
        vbatGain: parseFloat(document.getElementById('diag-calib-vbat-gain').value) || 0,
        vbatOffsetMv: parseInt(document.getElementById('diag-calib-vbat-offset').value, 10) || 0,
    };
}

function unauthTest() {
    const el = document.getElementById('unauth-result');
    if (state.authedSlot >= 0) {
        el.className = 'neutral';
        el.textContent = 'session already authenticated — reconnect to retest the unauthenticated path';
        return;
    }
    const marker = (r) => {
        el.className = r === 1 ? 'pass' : 'fail';
        el.textContent = r === 1
            ? 'PASS — rejected (UNAUTHENTICATED), as required'
            : `FAIL — expected UNAUTHENTICATED, got ${resultName(r)}`;
    };
    const handler = (ch, op, p) => {
        if (ch === MC_CH.COMMAND && op === MC_OP.COMMAND_RESULT) marker(p[1]);
    };
    onceFrame(handler);
    send(MC_CH.COMMAND, MC_OP.SET_OUTPUT, new Uint8Array([0, 1]));
}

let onceHandlers = [];
function onceFrame(fn) { onceHandlers.push(fn); }
const _onFrameOrig = onFrame;
onFrame = function (bytes) {
    _onFrameOrig(bytes);
    if (onceHandlers.length && bytes.length >= 2) {
        const hs = onceHandlers; onceHandlers = [];
        for (const h of hs) h(bytes[0], bytes[1], bytes.subarray(2));
    }
};

// ---- CONFIG ----

function configRead() {
    state.configChunks = {};
    send(MC_CH.CONFIG, MC_OP.CONFIG_READ, new Uint8Array(0));
}

function onConfigFrame(op, p) {
    if (op === MC_OP.CONFIG_CHUNK) {
        const offset = readU16le(p, 0), total = readU16le(p, 2), data = p.subarray(4);
        if (total === 0) { document.getElementById('config-text').value = '(empty / unavailable)'; return; }
        state.configChunks[offset] = data;
        const got = Object.values(state.configChunks).reduce((n, d) => n + d.length, 0);
        if (got >= total) {
            const offsets = Object.keys(state.configChunks).map(Number).sort((a, b) => a - b);
            const buf = new Uint8Array(total);
            for (const off of offsets) buf.set(state.configChunks[off], off);
            document.getElementById('config-text').value = new TextDecoder().decode(buf);
            logLocal(`config read: ${total} bytes`);
        }
        return;
    }
    if (op === MC_OP.CONFIG_WRITE_RESULT) {
        logLocal(`config write: ${resultName(p[0])}`);
        return;
    }
}

function configWriteAll(jsonText) {
    const bytes = new TextEncoder().encode(jsonText);
    if (bytes.length === 0 || bytes.length > CONFIG_JSON_MAX) {
        logLocal(`config write: refused locally, ${bytes.length} bytes (max ${CONFIG_JSON_MAX})`);
        return;
    }
    send(MC_CH.CONFIG, MC_OP.CONFIG_WRITE_BEGIN, u16le(bytes.length));
    for (let off = 0; off < bytes.length; off += CONFIG_CHUNK) {
        const chunk = bytes.subarray(off, Math.min(off + CONFIG_CHUNK, bytes.length));
        send(MC_CH.CONFIG, MC_OP.CONFIG_WRITE_CHUNK, concatBytes(u16le(off), chunk));
    }
    send(MC_CH.CONFIG, MC_OP.CONFIG_WRITE_COMMIT, new Uint8Array(0));
}

// ---- OTA ----
//
// docs/PROTOCOL.md §10. This panel doesn't take a real firmware file (the
// sim's flash HAL is just an in-memory buffer, firmware/sim/src/main.c) --
// it generates `sizeBytes` of synthetic image data locally, signs it with
// SIM_OTA_TEST_SECRET_KEY (above), and drives the full begin/chunk-loop/
// commit sequence, the same thing firmware/sim/itest/moto-client.mjs's
// otaTransfer() proves against a spawned sim in CI. Useful here for
// interactively exercising safe-state gating (toggle "engine running"
// first) and watching OTA_STATUS/the event log update live.

function otaAwait(matchOp) {
    return new Promise((resolve) => {
        onceFrame((ch, op, p) => { if (ch === MC_CH.OTA && op === matchOp) resolve(p); });
    });
}

async function otaTransferTest(sizeBytes) {
    const el = document.getElementById('ota-result');
    const image = new Uint8Array(sizeBytes);
    crypto.getRandomValues(image);
    const digest = nacl.hash(image); // SHA-512, 64 bytes
    const signature = nacl.sign.detached(digest, SIM_OTA_TEST_SECRET_KEY);

    el.textContent = `beginning (${sizeBytes} bytes)…`;
    el.className = 'neutral';
    send(MC_CH.OTA, MC_OP.OTA_BEGIN, concatBytes(u32le(sizeBytes), digest, signature));
    let p = await otaAwait(MC_OP.OTA_RESULT);
    if (p[0] !== 0) {
        el.textContent = `OTA_BEGIN: ${resultName(p[0])}`;
        el.className = 'fail';
        return;
    }

    for (let off = 0; off < image.length; off += OTA_CHUNK_BYTES) {
        const chunk = image.subarray(off, Math.min(off + OTA_CHUNK_BYTES, image.length));
        el.textContent = `chunk ${off}/${sizeBytes}…`;
        send(MC_CH.OTA, MC_OP.OTA_CHUNK, concatBytes(u32le(off), chunk));
        p = await otaAwait(MC_OP.OTA_RESULT);
        if (p[0] !== 0) {
            el.textContent = `OTA_CHUNK@${off}: ${resultName(p[0])}`;
            el.className = 'fail';
            return;
        }
    }

    send(MC_CH.OTA, MC_OP.OTA_COMMIT, new Uint8Array(0));
    p = await otaAwait(MC_OP.OTA_RESULT);
    el.textContent = `OTA_COMMIT: ${resultName(p[0])}`;
    el.className = p[0] === 0 ? 'pass' : 'fail';
    logLocal(`OTA transfer (${sizeBytes} bytes): ${resultName(p[0])}`);
    otaStatus();
}

function otaAbort() { send(MC_CH.OTA, MC_OP.OTA_ABORT, new Uint8Array(0)); }
function otaReboot() { send(MC_CH.OTA, MC_OP.OTA_REBOOT, new Uint8Array(0)); }
function otaStatus() { send(MC_CH.OTA, MC_OP.OTA_STATUS, new Uint8Array(0)); }

function onOtaFrame(op, p) {
    if (op === MC_OP.OTA_RESULT) {
        if (p[0] !== 0) logLocal(`OTA result: ${resultName(p[0])}`);
        return;
    }
    if (op === MC_OP.OTA_STATUS_RESULT) {
        const el = document.getElementById('ota-status');
        if (!el) return;
        const state_ = OTA_STATE[p[1]] || `?(${p[1]})`;
        const received = readU32le(p, 2), total = readU32le(p, 6);
        el.textContent = `${state_} — ${received}/${total} bytes`;
        return;
    }
}

// ---- Event log ----
//
// docs/PROTOCOL.md §15. EVENT_LOG_GET rides the COMMAND channel (same
// authenticated-session gate as SET_OUTPUT/lock/diagnostics), reassembled
// the same chunked-transfer way as CONFIG_CHUNK above.
let eventLogChunks = null;

function eventLogGet(sinceSeq) {
    eventLogChunks = { records: [], total: null };
    send(MC_CH.COMMAND, MC_OP.EVENT_LOG_GET, u32le(sinceSeq || 0));
}

function onEventLogChunk(p) {
    if (!eventLogChunks) eventLogChunks = { records: [], total: null };
    const total = readU16le(p, 2);
    const count = p[4];
    let pos = 5;
    for (let i = 0; i < count; i++) {
        const seq = readU32le(p, pos); pos += 4;
        const uptimeMs = readU32le(p, pos); pos += 4;
        const type = p[pos++], arg0 = p[pos++], arg1 = p[pos++];
        pos += 1; // reserved
        eventLogChunks.records.push({ seq, uptimeMs, type, arg0, arg1 });
    }
    eventLogChunks.total = total;
    if (eventLogChunks.records.length >= total) renderEventLog(eventLogChunks.records);
}

function renderEventLog(records) {
    const el = document.getElementById('event-log-list');
    if (!el) return;
    if (records.length === 0) {
        el.textContent = '(empty)';
        return;
    }
    el.innerHTML = records
        .map((r) => {
            const name = EVENT_TYPE_NAMES[r.type] || `?(${r.type})`;
            return `<div>#${r.seq} @${r.uptimeMs}ms ${name} arg0=${r.arg0}</div>`;
        })
        .join('');
}

// ---- sim debug channel ----

function debugSetBattery(mv) { send(SIM_CH_DEBUG, SIM_OP.SET_BATTERY_MV, u16le(mv)); }
function debugSetChannelFault(channel, currentMa, fault) {
    send(SIM_CH_DEBUG, SIM_OP.SET_CHANNEL_FAULT, concatBytes(new Uint8Array([channel]), u16le(currentMa), new Uint8Array([fault])));
}
function debugSetEngineRunning(on) { send(SIM_CH_DEBUG, SIM_OP.SET_ENGINE_RUNNING, new Uint8Array([on ? 1 : 0])); }
function debugSetInterlock(on) { send(SIM_CH_DEBUG, SIM_OP.SET_INTERLOCK, new Uint8Array([on ? 1 : 0])); }
function debugButtonState(button, pressed) { send(SIM_CH_DEBUG, SIM_OP.BUTTON_STATE, new Uint8Array([button, pressed ? 1 : 0])); }
function debugForceDisconnect() { send(SIM_CH_DEBUG, SIM_OP.FORCE_DISCONNECT, new Uint8Array(0)); }
function debugForceReboot() { send(SIM_CH_DEBUG, SIM_OP.FORCE_REBOOT, new Uint8Array(0)); }
function debugForceNvsCorrupt(target) { send(SIM_CH_DEBUG, SIM_OP.FORCE_NVS_CORRUPT, new Uint8Array([target])); }
function debugResetFaults() { send(SIM_CH_DEBUG, SIM_OP.RESET_FAULTS, new Uint8Array(0)); }

function onDebugFrame(op, p) {
    if (op === SIM_OP.LOG_ENTRY) {
        const tMs = readU32le(p, 0), textLen = p[4];
        const text = new TextDecoder().decode(p.subarray(5, 5 + textLen));
        appendLog(tMs, text);
        return;
    }
    if (op === SIM_OP.STATE) {
        const batteryMv = readU16le(p, 0);
        const engineRunning = !!p[2], interlock = !!p[3];
        document.getElementById('sl-battery').value = batteryMv;
        document.getElementById('sl-battery-val').textContent = (batteryMv / 1000).toFixed(1) + ' V';
        document.getElementById('chk-engine-running').checked = engineRunning;
        document.getElementById('chk-interlock').checked = interlock;
        let o = 4;
        for (let c = 0; c < OUTPUT_COUNT; c++) {
            state.channelSim[c] = { current: readU16le(p, o), fault: p[o + 2] };
            o += 3;
        }
        updateChannelsDisplay();
        return;
    }
    if (op === SIM_OP.ACK) return; // informational only; setters already reflect via STATE/STATUS pushes
}

// ---- event log ----

function appendLog(tMs, text) {
    const el = document.getElementById('log');
    const row = document.createElement('div');
    row.className = 'entry';
    const t = document.createElement('span');
    t.className = 't';
    t.textContent = `[${(tMs / 1000).toFixed(3)}s]`;
    row.appendChild(t);
    row.appendChild(document.createTextNode(text));
    el.appendChild(row);
    el.scrollTop = el.scrollHeight;
}
function logLocal(text) { appendLog(state.status ? state.status.uptimeMs : 0, '(gui) ' + text); }

// ---- rendering ----

function renderStatus() {
    const s = state.status;
    if (!s) return;
    document.getElementById('st-fw').textContent = s.fw;
    document.getElementById('st-lock').textContent = LOCK_STATE[s.lockState] || s.lockState;
    document.getElementById('st-uptime').textContent = (s.uptimeMs / 1000).toFixed(1) + ' s';
    document.getElementById('st-batt').textContent = (s.batteryMv / 1000).toFixed(2) + ' V';
    document.getElementById('st-rssi').textContent = s.rssi + ' dBm';
    document.getElementById('st-fault').textContent = '0b' + s.faultMask.toString(2).padStart(OUTPUT_COUNT, '0');
    const backoffEl = document.getElementById('st-backoff');
    backoffEl.textContent = s.cheatcodeBackoff ? 'BACKOFF' : 'ready';
    backoffEl.className = 'value ' + (s.cheatcodeBackoff ? 'fail' : '');
    const cutoffEl = document.getElementById('st-cutoff');
    cutoffEl.textContent = s.lvCutoffActive ? 'ACTIVE' : 'normal';
    cutoffEl.className = 'value ' + (s.lvCutoffActive ? 'fail' : '');
}

function buildChannelsDom() {
    const root = document.getElementById('channels');
    root.innerHTML = '';
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        const div = document.createElement('div');
        div.className = 'channel';
        div.id = `ch-${c}`;
        div.innerHTML = `
            <div class="row"><span class="idx">CH ${c}</span>
                <label class="switch"><input type="checkbox" id="ch-${c}-sw"><span class="slider"></span></label>
            </div>
            <div class="row"><span>current (mA)</span><input type="number" id="ch-${c}-cur" min="0" max="65000" value="0"></div>
            <span class="fault-badge" id="ch-${c}-fault">none</span>`;
        root.appendChild(div);
        div.querySelector(`#ch-${c}-sw`).addEventListener('change', (e) => {
            dispatchAction('setOutput', { channel: c, on: e.target.checked });
        });
        div.querySelector(`#ch-${c}-cur`).addEventListener('change', (e) => {
            const cur = parseInt(e.target.value, 10) || 0;
            // fault is always sent as 0/ignored -- mc_diag derives the real
            // fault from this current against the channel's configured
            // thresholds (Diagnostics panel), it's never set directly.
            dispatchAction('setChannelFault', { channel: c, current: cur, fault: 0 });
        });
    }
}

function updateChannelsDisplay() {
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        const div = document.getElementById(`ch-${c}`);
        if (!div) continue;
        div.classList.toggle('on', !!state.outputOn[c]);
        const sw = document.getElementById(`ch-${c}-sw`);
        if (sw && document.activeElement !== sw) sw.checked = !!state.outputOn[c];
        const cur = document.getElementById(`ch-${c}-cur`);
        if (cur && document.activeElement !== cur) cur.value = state.channelSim[c].current;
        const fault = document.getElementById(`ch-${c}-fault`);
        if (fault) {
            const f = state.channelSim[c].fault;
            fault.textContent = FAULT_NAMES[f] || `?(${f})`;
            fault.classList.toggle('fault', f !== 0);
        }
    }
}

// ---- Diagnostics panel DOM ----

function buildDiagChannelsDom() {
    const root = document.getElementById('diag-channels');
    root.innerHTML = '';
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        const div = document.createElement('div');
        div.className = 'channel';
        div.innerHTML = `
            <div class="row"><span class="idx">CH ${c}</span>
                <button class="small" id="dch-${c}-learn">Learn</button></div>
            <div class="row"><span>open-load (mA)</span><input type="number" id="dch-${c}-open" min="0" max="65535" value="50"></div>
            <div class="row"><span>overcurrent (mA)</span><input type="number" id="dch-${c}-over" min="0" max="65535" value="15000"></div>`;
        root.appendChild(div);
        div.querySelector(`#dch-${c}-learn`).addEventListener('click', () => dispatchAction('diagLearn', { channel: c }));
    }
}

function renderDiagConfig() {
    const cfg = state.diagConfig;
    if (!cfg) return;
    for (let c = 0; c < OUTPUT_COUNT; c++) {
        const open = document.getElementById(`dch-${c}-open`);
        const over = document.getElementById(`dch-${c}-over`);
        if (open && document.activeElement !== open) open.value = cfg.channels[c].openLoadMa;
        if (over && document.activeElement !== over) over.value = cfg.channels[c].overcurrentMa;
    }
    const setIfIdle = (id, v) => {
        const el = document.getElementById(id);
        if (el && document.activeElement !== el) el.value = v;
    };
    setIfIdle('diag-lv-cutoff-mv', cfg.lvCutoffMv);
    setIfIdle('diag-lv-cutoff-hyst-mv', cfg.lvCutoffHysteresisMv);
    setIfIdle('diag-engine-run-mv', cfg.engineRunMv);
    setIfIdle('diag-engine-run-hyst-mv', cfg.engineRunHysteresisMv);
    const detectEl = document.getElementById('diag-engine-run-detect-enabled');
    if (detectEl && document.activeElement !== detectEl) detectEl.checked = cfg.engineRunVoltageDetectionEnabled;
    document.getElementById('diag-config-summary').textContent =
        `cutoff ${(cfg.lvCutoffMv / 1000).toFixed(1)}V (+${cfg.lvCutoffHysteresisMv}mV) · ` +
        `engine-run ${(cfg.engineRunMv / 1000).toFixed(1)}V (+${cfg.engineRunHysteresisMv}mV)` +
        (cfg.engineRunVoltageDetectionEnabled ? '' : ' [voltage detection OFF]');
}

function renderDiagCalib() {
    const c = state.diagCalib;
    if (!c) return;
    const setIfIdle = (id, v) => {
        const el = document.getElementById(id);
        if (el && document.activeElement !== el) el.value = v;
    };
    setIfIdle('diag-calib-is-gain', c.isGain);
    setIfIdle('diag-calib-is-offset', c.isOffsetMv);
    setIfIdle('diag-calib-kilis', c.kilis);
    setIfIdle('diag-calib-vbat-gain', c.vbatGain);
    setIfIdle('diag-calib-vbat-offset', c.vbatOffsetMv);
}

function buildButtonsDom() {
    const root = document.getElementById('vbuttons');
    root.innerHTML = '';
    for (let i = 0; i < INPUT_COUNT; i++) {
        const div = document.createElement('div');
        div.className = 'vbutton';
        div.id = `btn-${i}`;
        div.textContent = `BTN${i}`;
        const press = (down) => (e) => {
            e.preventDefault();
            dispatchAction('buttonState', { button: i, pressed: down });
            div.classList.toggle('pressed', down);
        };
        div.addEventListener('mousedown', press(true));
        div.addEventListener('mouseup', press(false));
        div.addEventListener('mouseleave', () => { if (div.classList.contains('pressed')) press(false)(new Event('leave')); });
        div.addEventListener('touchstart', press(true));
        div.addEventListener('touchend', press(false));
        root.appendChild(div);
    }
}

function renderPhones() {
    const root = document.getElementById('phones');
    root.innerHTML = '';
    for (const phone of state.phones) {
        const row = document.createElement('div');
        row.className = 'phone-row';
        const authed = phone.slot !== null && phone.slot === state.authedSlot;
        row.innerHTML = `
            <span class="name">${phone.label}</span>
            <span class="slot">${phone.slot !== null ? 'slot ' + phone.slot : 'not enrolled'}</span>
            <span class="${authed ? 'pass' : 'neutral'}">${authed ? 'AUTHENTICATED' : ''}</span>
            <span class="grow"></span>
            <button class="small" data-act="enroll">Enroll</button>
            <button class="small" data-act="auth">Authenticate</button>
            <button class="small danger" data-act="revoke" ${phone.slot === null ? 'disabled' : ''}>Revoke</button>`;
        row.querySelector('[data-act=enroll]').onclick = () => dispatchAction('enroll', { label: phone.label });
        row.querySelector('[data-act=auth]').onclick = () => dispatchAction('authBegin', { label: phone.label });
        row.querySelector('[data-act=revoke]').onclick = () => { if (phone.slot !== null) dispatchAction('revoke', { slot: phone.slot }); };
        root.appendChild(row);
    }
}

// ---- scenario recorder ----

const ACTIONS = {
    setOutput: ({ channel, on }) => cmdSetOutput(channel, on),
    authBegin: ({ label }) => { const p = state.phones.find((x) => x.label === label); if (p) authBegin(p); },
    enroll: ({ label }) => { const p = state.phones.find((x) => x.label === label); if (p) enroll(p); },
    revoke: ({ slot }) => keyRevoke(slot),
    addPhone: ({ label }) => addPhone(label),
    setBattery: ({ mv }) => { debugSetBattery(mv); },
    setChannelFault: ({ channel, current, fault }) => debugSetChannelFault(channel, current, fault),
    setEngineRunning: ({ on }) => debugSetEngineRunning(on),
    setInterlock: ({ on }) => debugSetInterlock(on),
    buttonState: ({ button, pressed }) => debugButtonState(button, pressed),
    forceDisconnect: () => debugForceDisconnect(),
    forceReboot: () => debugForceReboot(),
    corruptNvs: ({ target }) => debugForceNvsCorrupt(target),
    resetFaults: () => debugResetFaults(),
    configRead: () => configRead(),
    configWrite: ({ json }) => configWriteAll(json),
    otaTransferTest: ({ sizeBytes }) => otaTransferTest(sizeBytes),
    otaAbort: () => otaAbort(),
    otaReboot: () => otaReboot(),
    otaStatus: () => otaStatus(),
    eventLogGet: ({ sinceSeq }) => eventLogGet(sinceSeq),
    unauthTest: () => unauthTest(),
    lock: () => cmdLock(),
    unlock: () => cmdUnlock(),
    lockGetConfig: () => cmdLockGetConfig(),
    lockSetConfig: (args) => cmdLockSetConfig(args),
    cheatcodeSet: ({ buttons }) => cmdCheatcodeSet(buttons),
    cheatcodeClear: () => cmdCheatcodeClear(),
    cheatcodeTest: ({ buttons }) => cmdCheatcodeTest(buttons),
    transferOwnership: () => cmdTransferOwnership(),
    diagGet: () => cmdDiagGet(),
    diagGetConfig: () => cmdDiagGetConfig(),
    diagSetConfig: (cfg) => cmdDiagSetConfig(cfg),
    diagGetCalib: () => cmdDiagGetCalib(),
    diagSetCalib: (calib) => cmdDiagSetCalib(calib),
    diagLearn: ({ channel }) => cmdDiagLearn(channel),
    hazardPress: () => cmdHazardPress(),
};

function dispatchAction(name, args) {
    args = args || {};
    if (state.recording) {
        state.recordBuf.push({ t: Date.now() - state.recordStart, name, args });
        document.getElementById('rec-status').textContent = `recording... ${state.recordBuf.length} actions`;
    }
    const fn = ACTIONS[name];
    if (fn) fn(args);
}

function replayScenario(scenario) {
    logLocal(`replaying scenario: ${scenario.actions.length} actions`);
    for (const a of scenario.actions) {
        setTimeout(() => {
            logLocal(`replay: ${a.name} ${JSON.stringify(a.args)}`);
            const fn = ACTIONS[a.name];
            if (fn) fn(a.args);
        }, a.t);
    }
}

// ---- phones ----

function addPhone(label) {
    const kp = nacl.sign.keyPair();
    state.phones.push({ label, publicKey: kp.publicKey, secretKey: kp.secretKey, slot: null });
    savePhones();
    renderPhones();
}

// ---- wiring ----

window.addEventListener('DOMContentLoaded', () => {
    loadPhones();
    buildChannelsDom();
    buildDiagChannelsDom();
    buildButtonsDom();
    renderPhones();

    document.getElementById('btn-connect').onclick = () => {
        state.autoReconnect = document.getElementById('chk-autoreconnect').checked;
        connect(document.getElementById('conn-url').value.trim());
    };
    document.getElementById('btn-disconnect').onclick = disconnect;
    document.getElementById('chk-autoreconnect').onchange = (e) => { state.autoReconnect = e.target.checked; };

    document.getElementById('sl-battery').addEventListener('input', (e) => {
        document.getElementById('sl-battery-val').textContent = (e.target.value / 1000).toFixed(1) + ' V';
    });
    document.getElementById('sl-battery').addEventListener('change', (e) => {
        dispatchAction('setBattery', { mv: parseInt(e.target.value, 10) });
    });
    document.getElementById('chk-engine-running').addEventListener('change', (e) => {
        dispatchAction('setEngineRunning', { on: e.target.checked });
    });
    document.getElementById('chk-interlock').addEventListener('change', (e) => {
        dispatchAction('setInterlock', { on: e.target.checked });
    });

    document.getElementById('btn-add-phone').onclick = () => {
        const input = document.getElementById('phone-label');
        const label = input.value.trim() || `phone-${state.phones.length + 1}`;
        dispatchAction('addPhone', { label });
        input.value = '';
    };
    document.getElementById('btn-unauth-test').onclick = () => dispatchAction('unauthTest', {});

    document.getElementById('btn-hazard-press').onclick = () => dispatchAction('hazardPress', {});

    document.getElementById('btn-lk-lock').onclick = () => dispatchAction('lock', {});
    document.getElementById('btn-lk-unlock').onclick = () => dispatchAction('unlock', {});
    document.getElementById('btn-lk-get-config').onclick = () => dispatchAction('lockGetConfig', {});
    document.getElementById('btn-lk-save-config').onclick = () => {
        dispatchAction('lockSetConfig', {
            enabled: document.getElementById('lk-enabled').checked,
            methodsMask:
                (document.getElementById('lk-method-phone').checked ? LOCK_METHOD.PHONE : 0) |
                (document.getElementById('lk-method-ignswitch').checked ? LOCK_METHOD.IGNITION_SWITCH : 0),
            ignitionSwitchInput: parseInt(document.getElementById('lk-ignswitch-input').value, 10),
            graceMs: parseInt(document.getElementById('lk-grace-ms').value, 10) || 0,
            windowMs: parseInt(document.getElementById('lk-window-ms').value, 10) || 0,
        });
    };
    document.getElementById('btn-lk-set-code').onclick = () => {
        const buttons = parseCodeInput(document.getElementById('lk-code-input').value);
        if (buttons.length < 4 || buttons.length > 10) {
            logLocal(`cheat-code set: refused locally, need 4-10 button indices (0-7), got ${buttons.length}`);
            return;
        }
        dispatchAction('cheatcodeSet', { buttons });
    };
    document.getElementById('btn-lk-clear-code').onclick = () => dispatchAction('cheatcodeClear', {});
    document.getElementById('btn-lk-test-code').onclick = () => {
        const buttons = parseCodeInput(document.getElementById('lk-code-input').value);
        dispatchAction('cheatcodeTest', { buttons });
    };
    document.getElementById('btn-lk-transfer').onclick = () => {
        if (confirm('Transfer ownership: wipes ALL enrolled keys and the cheat-code/lock config. Continue?')) {
            dispatchAction('transferOwnership', {});
        }
    };

    document.getElementById('btn-diag-get-config').onclick = () => dispatchAction('diagGetConfig', {});
    document.getElementById('btn-diag-save-config').onclick = () => dispatchAction('diagSetConfig', readDiagConfigFromDom());
    document.getElementById('btn-diag-learn-all').onclick = () => dispatchAction('diagLearn', { channel: 0xff });
    document.getElementById('btn-diag-get-calib').onclick = () => dispatchAction('diagGetCalib', {});
    document.getElementById('btn-diag-save-calib').onclick = () => dispatchAction('diagSetCalib', readDiagCalibFromDom());

    document.getElementById('btn-force-disconnect').onclick = () => dispatchAction('forceDisconnect', {});
    document.getElementById('btn-force-reboot').onclick = () => dispatchAction('forceReboot', {});
    document.getElementById('btn-reset-faults').onclick = () => dispatchAction('resetFaults', {});
    document.getElementById('btn-corrupt-nvs').onclick = () => {
        dispatchAction('corruptNvs', { target: parseInt(document.getElementById('nvs-target').value, 10) });
    };
    document.getElementById('btn-ota-transfer').onclick = () => {
        const sizeKb = parseInt(document.getElementById('ota-size-kb').value, 10) || 16;
        dispatchAction('otaTransferTest', { sizeBytes: sizeKb * 1024 });
    };
    document.getElementById('btn-ota-abort').onclick = () => dispatchAction('otaAbort', {});
    document.getElementById('btn-ota-reboot').onclick = () => dispatchAction('otaReboot', {});
    document.getElementById('btn-ota-status').onclick = () => dispatchAction('otaStatus', {});
    document.getElementById('btn-event-log-get').onclick = () => dispatchAction('eventLogGet', { sinceSeq: 0 });

    document.getElementById('btn-config-read').onclick = () => dispatchAction('configRead', {});
    document.getElementById('btn-config-write').onclick = () => {
        dispatchAction('configWrite', { json: document.getElementById('config-text').value });
    };
    document.getElementById('btn-config-download').onclick = () => {
        const blob = new Blob([document.getElementById('config-text').value], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = 'moto-ctrl-config.json';
        a.click();
    };
    document.getElementById('config-file').addEventListener('change', (e) => {
        const file = e.target.files[0];
        if (!file) return;
        file.text().then((text) => { document.getElementById('config-text').value = text; });
    });

    document.getElementById('btn-rec-start').onclick = () => {
        state.recording = true;
        state.recordStart = Date.now();
        state.recordBuf = [];
        document.getElementById('btn-rec-start').disabled = true;
        document.getElementById('btn-rec-stop').disabled = false;
        document.getElementById('btn-rec-save').disabled = true;
        document.getElementById('rec-status').textContent = 'recording...';
    };
    document.getElementById('btn-rec-stop').onclick = () => {
        state.recording = false;
        document.getElementById('btn-rec-start').disabled = false;
        document.getElementById('btn-rec-stop').disabled = true;
        document.getElementById('btn-rec-save').disabled = state.recordBuf.length === 0;
        document.getElementById('rec-status').textContent = `stopped — ${state.recordBuf.length} actions captured`;
    };
    document.getElementById('btn-rec-save').onclick = () => {
        const blob = new Blob([JSON.stringify({ version: 1, actions: state.recordBuf }, null, 2)], { type: 'application/json' });
        const a = document.createElement('a');
        a.href = URL.createObjectURL(blob);
        a.download = `moto-ctrl-scenario-${Date.now()}.json`;
        a.click();
    };
    document.getElementById('btn-scenario-load').onclick = () => {
        const file = document.getElementById('scenario-file').files[0];
        if (!file) return;
        file.text().then((text) => replayScenario(JSON.parse(text)));
    };

    document.getElementById('btn-clear-log').onclick = () => { document.getElementById('log').innerHTML = ''; };

    // current/fault/battery/engine-running are REAL, derived
    // values that change on their own (round-robin sampling, cutoff/engine
    // hysteresis) rather than only when the user edits them — poll GET_STATE
    // periodically so the display doesn't go stale between manual actions.
    setInterval(() => {
        if (state.connected) send(SIM_CH_DEBUG, SIM_OP.GET_STATE, new Uint8Array(0));
    }, 1000);

    connect(state.url);
});
