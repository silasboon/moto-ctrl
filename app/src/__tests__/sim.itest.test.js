/**
 * Live integration test: spawns a real firmware/sim/build/moto_ctrl_sim
 * process and drives it through the real SimTransport + MotoClient — the
 * strongest signal available without hardware, since it's the same
 * simulator firmware/sim/itest/*.mjs and the debug GUI already prove the
 * protocol against (see docs/TESTING.md).
 *
 * Plain JS (not TS): keeps this test independent of the app's strict
 * RN-flavored tsconfig (no Node lib/types included there), matching
 * firmware/sim/itest/*.mjs's own plain-JS style. The RN-native WebSocket
 * global SimTransport needs is polyfilled for every test file by
 * jest.config.js's setupFiles (jest.setup.js), not here.
 *
 * Skips (doesn't fail) if the sim hasn't been built locally:
 *   cd firmware/sim && cmake -S . -B build && cmake --build build
 * CI builds it first (see .github/workflows/app.yml).
 */
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');

const nacl = require('tweetnacl');
const { MotoClient } = require('../protocol/MotoClient');
const { SimTransport } = require('../transport/SimTransport');

const SIM_BIN =
  process.env.MOTO_SIM_BIN ||
  path.resolve(__dirname, '../../../firmware/sim/build/moto_ctrl_sim');
const HAVE_SIM = fs.existsSync(SIM_BIN);

let nextPort = 8300 + Math.floor(Math.random() * 500);

function startSim() {
  const port = nextPort++;
  const sim = spawn(SIM_BIN, [String(port)], {
    stdio: ['ignore', 'pipe', 'inherit'],
  });
  const ready = new Promise((resolve, reject) => {
    const timer = setTimeout(
      () => reject(new Error('sim did not start listening in time')),
      5000,
    );
    sim.stdout.on('data', chunk => {
      if (chunk.toString().includes('listening')) {
        clearTimeout(timer);
        resolve();
      }
    });
    sim.on('error', reject);
  });
  return { sim, port, ready };
}

async function enrollAndAuth(client) {
  const kp = nacl.sign.keyPair();
  const enroll = await client.enroll(kp.publicKey, 'Test Phone');
  expect(enroll.ok).toBe(true);
  const auth = await client.authenticate({
    publicKey: kp.publicKey,
    secretKey: kp.secretKey,
  });
  expect(auth.ok).toBe(true);
  return kp;
}

const d = HAVE_SIM ? describe : describe.skip;
if (!HAVE_SIM) {
  // eslint-disable-next-line no-console
  console.warn(
    `Skipping live sim tests: ${SIM_BIN} not found. Build firmware/sim first (see docs/TESTING.md).`,
  );
}

d('MotoClient against a real firmware/sim instance', () => {
  let sim;
  let client;

  beforeEach(async () => {
    const started = startSim();
    sim = started.sim;
    await started.ready;
    client = new MotoClient(new SimTransport(`ws://127.0.0.1:${started.port}`));
    await client.connect(`ws://127.0.0.1:${started.port}`);
  });

  afterEach(async () => {
    await client.disconnect().catch(() => {});
    sim.kill();
  });

  test('status is readable without authentication', async () => {
    const status = await client.getStatus();
    expect(status.fwMajor).toBe(0);
  });

  test('control is rejected before authentication, then succeeds after enroll+auth', async () => {
    const before = await client.setOutput(0, false);
    expect(before.ok).toBe(false);

    await enrollAndAuth(client);

    const after = await client.setOutput(0, false);
    expect(after.ok).toBe(true);
  });

  test('starter output is rejected over the wire even once authenticated', async () => {
    await enrollAndAuth(client);

    const config = await client.configRead();
    config.outputs.channels[0].is_starter = true;
    const write = await client.configWrite(config);
    expect(write.ok).toBe(true);

    const result = await client.setOutput(0, true);
    expect(result.ok).toBe(false);
  });

  test('config write/read round-trips a channel name, behaviour and roles', async () => {
    await enrollAndAuth(client);

    const config = await client.configRead();
    config.outputs.channels[3].name = 'Low Beam';
    config.outputs.channels[3].behaviour = 'toggle';
    // schema_version 6: "never shed this under low voltage" is an explicit
    // flag rather than something inferred from a headlight tag.
    config.outputs.channels[3].essential = true;
    const write = await client.configWrite(config);
    expect(write.ok).toBe(true);

    const reread = await client.configRead();
    expect(reread.outputs.channels[3].name).toBe('Low Beam');
    expect(reread.outputs.channels[3].behaviour).toBe('toggle');
    expect(reread.outputs.channels[3].essential).toBe(true);
  });

  test('two phones can be enrolled and authenticate independently; revoking one blocks it', async () => {
    const kpA = await enrollAndAuth(client);
    const kpB = nacl.sign.keyPair();
    const enrollB = await client.enroll(kpB.publicKey, 'Phone B');
    expect(enrollB.ok).toBe(true);

    const authB = await client.authenticate({
      publicKey: kpB.publicKey,
      secretKey: kpB.secretKey,
    });
    expect(authB.ok).toBe(true);

    const revoke = await client.keyRevoke(authB.slot);
    expect(revoke.ok).toBe(true);

    const authBAgain = await client.authenticate({
      publicKey: kpB.publicKey,
      secretKey: kpB.secretKey,
    });
    expect(authBAgain.ok).toBe(false);

    const authAAgain = await client.authenticate({
      publicKey: kpA.publicKey,
      secretKey: kpA.secretKey,
    });
    expect(authAAgain.ok).toBe(true);
  });

  describe('Lock / immobilizer', () => {
    async function configureIgnitionChannel() {
      const config = await client.configRead();
      config.outputs.channels[5].is_ignition = true;
      const write = await client.configWrite(config);
      expect(write.ok).toBe(true);
    }

    test('lock config round-trips and enabling requires a cheat-code first', async () => {
      await enrollAndAuth(client);
      await configureIgnitionChannel();

      const before = await client.lockGetConfig();
      expect(before.immobilizerEnabled).toBe(false);
      expect(before.cheatcodeSet).toBe(false);

      const rejected = await client.lockSetConfig({
        immobilizerEnabled: true,
        methodsMask: 1,
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });
      expect(rejected.ok).toBe(false); // no cheat-code set yet

      const set = await client.cheatcodeSet([0, 1, 2, 3]);
      expect(set.ok).toBe(true);

      const accepted = await client.lockSetConfig({
        immobilizerEnabled: true,
        methodsMask: 1,
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });
      expect(accepted.ok).toBe(true);

      const after = await client.lockGetConfig();
      expect(after.immobilizerEnabled).toBe(true);
      expect(after.cheatcodeSet).toBe(true);
      expect(after.cheatcodeLen).toBe(4);
    });

    test('lock rejects ignition, explicit unlock releases it', async () => {
      await enrollAndAuth(client);
      await configureIgnitionChannel();
      await client.cheatcodeSet([0, 1, 2, 3]);
      await client.lockSetConfig({
        immobilizerEnabled: true,
        methodsMask: 1, // PHONE
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });

      const lockRes = await client.lock();
      expect(lockRes.ok).toBe(true);
      const status1 = await client.getStatus();
      expect(status1.lockState).toBe(2); // LOCKED

      const ignOn = await client.setOutput(5, true);
      expect(ignOn.ok).toBe(false); // immobilized

      const unlockRes = await client.unlock();
      expect(unlockRes.ok).toBe(true);
      const status2 = await client.getStatus();
      expect(status2.lockState).not.toBe(2);

      const ignOnAgain = await client.setOutput(5, true);
      expect(ignOnAgain.ok).toBe(true);
    });

    test('cheatcodeTest is a pure practice check', async () => {
      await enrollAndAuth(client);
      await configureIgnitionChannel();
      await client.cheatcodeSet([1, 2, 3, 4]);

      const right = await client.cheatcodeTest([1, 2, 3, 4]);
      expect(right.ok).toBe(true);
      expect(right.match).toBe(true);

      const wrong = await client.cheatcodeTest([4, 3, 2, 1]);
      expect(wrong.match).toBe(false);
    });

    test('cheatcodeClear is rejected while the immobilizer is enabled', async () => {
      await enrollAndAuth(client);
      await configureIgnitionChannel();
      await client.cheatcodeSet([0, 1, 2, 3]);
      await client.lockSetConfig({
        immobilizerEnabled: true,
        methodsMask: 1,
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });

      const clearBlocked = await client.cheatcodeClear();
      expect(clearBlocked.ok).toBe(false);

      await client.lockSetConfig({
        immobilizerEnabled: false,
        methodsMask: 1,
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });
      const clearOk = await client.cheatcodeClear();
      expect(clearOk.ok).toBe(true);
    });

    test('ownership transfer wipes keys and lock config', async () => {
      const kp = await enrollAndAuth(client);
      await configureIgnitionChannel();
      await client.cheatcodeSet([0, 1, 2, 3]);
      await client.lockSetConfig({
        immobilizerEnabled: true,
        methodsMask: 1,
        ignitionSwitchInput: -1,
        autoLockGraceMs: 60000,
        cheatcodeWindowMs: 5000,
      });

      const xfer = await client.transferOwnership();
      expect(xfer.ok).toBe(true);

      const after = await client.lockGetConfig();
      expect(after.immobilizerEnabled).toBe(false);
      expect(after.cheatcodeSet).toBe(false);

      const authAgain = await client.authenticate({
        publicKey: kp.publicKey,
        secretKey: kp.secretKey,
      });
      expect(authAgain.ok).toBe(false); // key was wiped
    });
  });

  // These tests deliberately use only the real protocol (setOutput,
  // configRead/Write, diagGet*) — never the sim-only debug channel
  // (SIM_CH_DEBUG), which SimTransport/MotoClient never speak by design
  // (constants.ts). firmware/sim/itest/integration.test.mjs covers the
  // battery/current-injection scenarios that need it.
  describe('Diagnostics', () => {
    test('status reports a healthy battery and no cutoff by default', async () => {
      // mc_diag populates battery_mv from its first tick (~10ms after boot,
      // same as a real ADC not yet having been read once) -- give it a
      // moment so this doesn't race the connection handshake.
      await new Promise(r => setTimeout(r, 100));
      const status = await client.getStatus();
      expect(status.batteryMv).toBe(13200);
      expect(status.lvCutoffActive).toBe(false);
      expect(status.outputFaultMask).toBe(0);
    });

    test('diagGetConfig returns the real firmware defaults', async () => {
      await enrollAndAuth(client);
      const cfg = await client.diagGetConfig();
      expect(cfg.channels).toHaveLength(12);
      expect(cfg.channels[0]).toEqual({ openLoadMa: 50, overcurrentMa: 15000 });
      expect(cfg.lvCutoffMv).toBe(11800);
      expect(cfg.engineRunMv).toBe(13800);
    });

    test('diagSetConfig round-trips edited thresholds, and rejects inverted ones', async () => {
      await enrollAndAuth(client);
      const cfg = await client.diagGetConfig();
      cfg.channels[2] = { openLoadMa: 200, overcurrentMa: 4000 };
      cfg.lvCutoffMv = 12000;
      const setResult = await client.diagSetConfig(cfg);
      expect(setResult.ok).toBe(true);

      const reread = await client.diagGetConfig();
      expect(reread.channels[2]).toEqual({
        openLoadMa: 200,
        overcurrentMa: 4000,
      });
      expect(reread.lvCutoffMv).toBe(12000);

      reread.channels[2] = { openLoadMa: 9000, overcurrentMa: 100 }; // inverted
      const rejected = await client.diagSetConfig(reread);
      expect(rejected.ok).toBe(false);
    });

    test('an energized channel with no real current reads a real open-load fault', async () => {
      await enrollAndAuth(client);
      const config = await client.configRead();
      await client.configWrite(config);
      await client.setOutput(4, true);

      // The sim's real mc_diag round-robin needs a moment to sample it.
      await new Promise(r => setTimeout(r, 100));

      const diag = await client.getDiagnostics();
      expect(diag.channels[4].currentMa).toBe(0); // nothing injects current in this environment
      expect(diag.channels[4].fault).toBe(1); // DIAG_FAULT.OPEN_LOAD -- real classification, not mocked

      const status = await client.getStatus();
      // eslint-disable-next-line no-bitwise -- matches DashboardScreen's own outputFaultMask reads
      expect(status.outputFaultMask & (1 << 4)).not.toBe(0);
    });

    test('diagLearn requires the channel to be energized', async () => {
      await enrollAndAuth(client);
      const config = await client.configRead();
      await client.configWrite(config);

      const rejectedOff = await client.diagLearn(6);
      expect(rejectedOff.ok).toBe(false);

      await client.setOutput(6, true);
      await new Promise(r => setTimeout(r, 100));
      const learned = await client.diagLearn(6);
      expect(learned.ok).toBe(true);

      const cfg = await client.diagGetConfig();
      expect(cfg.channels[6].openLoadMa).toBe(0); // half of the measured 0mA
    });

    test('diagGetCalib / diagSetCalib round-trip real board calibration', async () => {
      await enrollAndAuth(client);
      const calib = await client.diagGetCalib();
      expect(calib.isGain).toBeCloseTo(1); // sim's identity default (see sim_nvs.h)

      const setResult = await client.diagSetCalib({ ...calib, isGain: 2.5 });
      expect(setResult.ok).toBe(true);

      const reread = await client.diagGetCalib();
      expect(reread.isGain).toBeCloseTo(2.5);
    });
  });

  describe('Flashers / PWM', () => {
    test('turn signals: setOutput applies mutual exclusion device-side', async () => {
      await enrollAndAuth(client);
      const config = await client.configRead();
      config.outputs.channels[0].indicator = 'left';
      config.outputs.channels[0].hazard_member = true;
      config.outputs.channels[1].indicator = 'right';
      config.outputs.channels[1].hazard_member = true;
      await client.configWrite(config);

      await client.setOutput(0, true);
      let status = await client.getStatus();
      // eslint-disable-next-line no-bitwise -- matches DashboardScreen's own outputStateMask reads
      expect(status.outputStateMask & (1 << 0)).not.toBe(0);

      await client.setOutput(1, true);
      status = await client.getStatus();
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 1)).not.toBe(0);
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 0)).toBe(0); // opposite side cancelled, with no client action
    });

    test('turn signals: auto-cancel timer clears the signal with no client action', async () => {
      await enrollAndAuth(client);
      const config = await client.configRead();
      config.outputs.channels[0].indicator = 'left';
      config.outputs.channels[0].hazard_member = true;
      config.outputs.turn_auto_cancel_ms = 300; // short, for a fast test
      await client.configWrite(config);

      await client.setOutput(0, true);
      let status = await client.getStatus();
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 0)).not.toBe(0);

      await new Promise(r => setTimeout(r, 600));
      status = await client.getStatus();
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 0)).toBe(0);
    });

    test('hazardPress toggles both turn channels together, rejected without any configured', async () => {
      await enrollAndAuth(client);
      const rejected = await client.hazardPress();
      expect(rejected.ok).toBe(false);

      const config = await client.configRead();
      config.outputs.channels[0].indicator = 'left';
      config.outputs.channels[0].hazard_member = true;
      config.outputs.channels[1].indicator = 'right';
      config.outputs.channels[1].hazard_member = true;
      await client.configWrite(config);

      const on = await client.hazardPress();
      expect(on.ok).toBe(true);
      let status = await client.getStatus();
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 0)).not.toBe(0);
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 1)).not.toBe(0);

      const off = await client.hazardPress();
      expect(off.ok).toBe(true);
      status = await client.getStatus();
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 0)).toBe(0);
      // eslint-disable-next-line no-bitwise
      expect(status.outputStateMask & (1 << 1)).toBe(0);
    });

    test('mode, pwm duty, and flasher timing round-trip through the real config JSON', async () => {
      await enrollAndAuth(client);
      const config = await client.configRead();
      config.outputs.channels[2].pwm_duty_pct = 42;
      config.outputs.brake_switch_input = 5;
      config.outputs.brake_flash_pulse_count = 5;
      await client.configWrite(config);

      const reread = await client.configRead();
      expect(reread.outputs.channels[2].behaviour).toBe('toggle');
      expect(reread.outputs.channels[2].pwm_duty_pct).toBe(42);
      expect(reread.outputs.brake_switch_input).toBe(5);
      expect(reread.outputs.brake_flash_pulse_count).toBe(5);
    });
  });
});
