/**
 * Unit tests for MotoClient against a small scripted fake Transport (no
 * network/BLE). These check MotoClient's own framing, request/reply
 * plumbing, and parsing. They intentionally do NOT re-verify the real
 * protocol's semantics (auth rules, config validation, starter rejection,
 * etc.) — that's the firmware's job, already proven by
 * firmware/sim/tests/test_session.c and firmware/sim/itest/. The live sim
 * integration test (sim.itest.test.ts) is the stronger signal for
 * cross-boundary correctness; keep both.
 */
import nacl from 'tweetnacl';

import {
  DIAG_LEARN_ALL,
  EVENT_TYPE,
  MC_CH,
  MC_OP,
  MC_RESULT,
  OTA_STATE,
  OUTPUT_COUNT,
} from '../protocol/constants';
import {
  concatBytes,
  f32le,
  i16le,
  u16le,
  u32le,
  utf8Encode,
} from '../protocol/frames';
import { MotoClient } from '../protocol/MotoClient';
import {
  defaultConfig,
  defaultDiagCalib,
  defaultDiagConfig,
} from '../protocol/types';
import type {
  ConnectionState,
  DeviceDescriptor,
  Transport,
} from '../transport/Transport';

type Emit = (payload: Uint8Array) => void;
type Responder = (
  channel: number,
  opcode: number,
  body: Uint8Array,
  emit: Emit,
) => void | Promise<void>;

class ScriptedTransport implements Transport {
  state: ConnectionState = 'disconnected';
  sentFrames: { channel: number; opcode: number; body: Uint8Array }[] = [];
  responder: Responder = () => {};
  private readonly stateListeners = new Set<(state: ConnectionState) => void>();
  private readonly messageListeners = new Set<
    (channel: number, data: Uint8Array) => void
  >();

  scan(onFound: (device: DeviceDescriptor) => void): () => void {
    onFound({ id: 'fake', name: 'fake' });
    return () => {};
  }

  async connect(): Promise<void> {
    this.state = 'connected';
    for (const l of this.stateListeners) l('connected');
  }

  async disconnect(): Promise<void> {
    this.state = 'disconnected';
    for (const l of this.stateListeners) l('disconnected');
  }

  getConnectionState(): ConnectionState {
    return this.state;
  }

  onConnectionStateChange(
    listener: (state: ConnectionState) => void,
  ): () => void {
    this.stateListeners.add(listener);
    return () => this.stateListeners.delete(listener);
  }

  onMessage(listener: (channel: number, data: Uint8Array) => void): () => void {
    this.messageListeners.add(listener);
    return () => this.messageListeners.delete(listener);
  }

  /** Simulates the link dropping without disconnect() being called — a board
   * going out of range or losing power. */
  dropConnection(): void {
    this.state = 'disconnected';
    for (const l of this.stateListeners) l('disconnected');
  }

  /** Pushes an unsolicited frame from the device, with no request behind it
   * (INPUT_EVENT). */
  emitOn(channel: number, payload: Uint8Array): void {
    for (const l of this.messageListeners) l(channel, payload);
  }

  /** Hook fired inside send(), for simulating a link that drops mid-request. */
  beforeSend: (() => void) | null = null;

  async send(channel: number, data: Uint8Array): Promise<void> {
    this.beforeSend?.();
    const opcode = data[0]!;
    const body = data.subarray(1);
    this.sentFrames.push({ channel, opcode, body });
    const emit: Emit = payload => {
      for (const l of this.messageListeners) l(channel, payload);
    };
    await this.responder(channel, opcode, body, emit);
  }
}

function frame(opcode: number, payload: Uint8Array): Uint8Array {
  return concatBytes(new Uint8Array([opcode]), payload);
}

function statusBytes(
  overrides: Partial<{
    batteryMv: number;
    outputStateMask: number;
    lockState: number;
    cheatcodeBackoff: boolean;
    lvCutoffActive: boolean;
  }> = {},
): Uint8Array {
  const b = new Uint8Array(16);
  b[0] = 0;
  b[1] = 4;
  b[2] = 0; // fw 0.4.0
  b[3] = overrides.lockState ?? 0; // lock UNKNOWN by default
  // uptime_ms (u32) left 0
  const battery = overrides.batteryMv ?? 12800;
  b[8] = battery & 0xff;
  b[9] = (battery >> 8) & 0xff;
  const mask = overrides.outputStateMask ?? 0;
  b[10] = mask & 0xff;
  b[11] = (mask >> 8) & 0xff;
  b[15] =
    (overrides.cheatcodeBackoff ? 0x01 : 0) |
    (overrides.lvCutoffActive ? 0x02 : 0);
  return b;
}

describe('MotoClient', () => {
  // MotoClient.connect() starts a status-poll interval; every test must
  // stop it via disconnect(), or the Jest worker leaks a live timer.
  const activeClients: MotoClient[] = [];
  afterEach(async () => {
    await Promise.all(activeClients.splice(0).map(c => c.disconnect()));
  });

  test('getStatus parses the 16-byte status wire', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(
          frame(
            MC_OP.STATUS,
            statusBytes({
              batteryMv: 13200,
              outputStateMask: 0b0000_0000_0101,
            }),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const status = await client.getStatus();
    expect(status.fwMinor).toBe(4);
    expect(status.batteryMv).toBe(13200);
    expect(status.outputStateMask).toBe(0b101);
  });

  test('authenticate signs the challenge and reports the returned slot', async () => {
    const keypair = nacl.sign.keyPair();
    const transport = new ScriptedTransport();
    const nonce = new Uint8Array(32).fill(7);
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.AUTH) return;
      if (opcode === MC_OP.AUTH_BEGIN) {
        emit(frame(MC_OP.AUTH_CHALLENGE, nonce));
      } else if (opcode === MC_OP.AUTH_RESPONSE) {
        const message = concatBytes(utf8Encode('moto-ctrl-auth-v1'), nonce);
        const valid = nacl.sign.detached.verify(
          message,
          body,
          keypair.publicKey,
        );
        emit(
          frame(
            MC_OP.AUTH_RESULT,
            new Uint8Array([
              valid ? MC_RESULT.OK : MC_RESULT.REJECTED,
              valid ? 3 : 0xff,
            ]),
          ),
        );
      }
    };

    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    expect(client.isAuthenticated()).toBe(false);

    const result = await client.authenticate(keypair);
    expect(result.ok).toBe(true);
    expect(result.slot).toBe(3);
    expect(client.isAuthenticated()).toBe(true);
  });

  test('authenticate reports failure for a signature from the wrong key', async () => {
    const enrolledKeypair = nacl.sign.keyPair();
    const wrongKeypair = nacl.sign.keyPair();
    const transport = new ScriptedTransport();
    const nonce = new Uint8Array(32).fill(9);
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.AUTH) return;
      if (opcode === MC_OP.AUTH_BEGIN) {
        emit(frame(MC_OP.AUTH_CHALLENGE, nonce));
      } else if (opcode === MC_OP.AUTH_RESPONSE) {
        const message = concatBytes(utf8Encode('moto-ctrl-auth-v1'), nonce);
        const valid = nacl.sign.detached.verify(
          message,
          body,
          enrolledKeypair.publicKey,
        );
        emit(
          frame(
            MC_OP.AUTH_RESULT,
            new Uint8Array([valid ? MC_RESULT.OK : MC_RESULT.REJECTED, 0xff]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const result = await client.authenticate(wrongKeypair);
    expect(result.ok).toBe(false);
    expect(result.result).toBe(MC_RESULT.REJECTED);
    expect(client.isAuthenticated()).toBe(false);
  });

  test('setOutput sends [channel, on] and reports the result', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.SET_OUTPUT) {
        expect(Array.from(body)).toEqual([5, 1]);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.SET_OUTPUT, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const result = await client.setOutput(5, true);
    expect(result.ok).toBe(true);
  });

  /* Two bytes since the cheat-code capture landed: enable, then whether the
   * device should also stop running the bindings for the presses it reports
   * (docs/PROTOCOL.md §14.1). Suppression defaults OFF, so identify-a-button
   * keeps behaving as it always did. */
  test.each<[string, boolean, boolean | undefined, number[]]>([
    ['enable only', true, undefined, [1, 0]],
    ['enable with suppression', true, true, [1, 1]],
    ['disable', false, undefined, [0, 0]],
  ])('inputLearn sends %s', async (_name, enable, suppress, expected) => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.INPUT_LEARN) {
        expect(Array.from(body)).toEqual(expected);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.INPUT_LEARN, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const result =
      suppress === undefined
        ? await client.inputLearn(enable)
        : await client.inputLearn(enable, suppress);
    expect(result.ok).toBe(true);
  });

  test('onInputEvent decodes button, press type and chord suppression', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const seen: unknown[] = [];
    const unsub = client.onInputEvent(e => seen.push(e));

    // press_type 0/1/2 = short/long/double; third byte is action_suppressed.
    transport.emitOn(
      MC_CH.COMMAND,
      frame(MC_OP.INPUT_EVENT, new Uint8Array([4, 0, 0])),
    );
    transport.emitOn(
      MC_CH.COMMAND,
      frame(MC_OP.INPUT_EVENT, new Uint8Array([0, 1, 0])),
    );
    transport.emitOn(
      MC_CH.COMMAND,
      frame(MC_OP.INPUT_EVENT, new Uint8Array([7, 2, 1])),
    );

    expect(seen).toEqual([
      { button: 4, pressType: 'short', actionSuppressed: false },
      { button: 0, pressType: 'long', actionSuppressed: false },
      { button: 7, pressType: 'double', actionSuppressed: true },
    ]);

    unsub();
    transport.emitOn(
      MC_CH.COMMAND,
      frame(MC_OP.INPUT_EVENT, new Uint8Array([1, 0, 0])),
    );
    expect(seen).toHaveLength(3);
  });

  /* INPUT_EVENT is unsolicited and shares the COMMAND channel, so it can
   * land between a request and its reply. It must not satisfy the pending
   * waiter — otherwise setOutput() would parse a button press as its
   * COMMAND_RESULT and report a bogus result code. */
  test('an unsolicited INPUT_EVENT does not resolve an in-flight command reply', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.SET_OUTPUT) {
        /* A press arrives first, then the real reply. press_type is
         * deliberately 1 (long), so the event's byte 1 is 1 — i.e.
         * UNAUTHENTICATED if it were ever misread as a COMMAND_RESULT. With
         * press_type 0 those bytes would coincidentally decode as OK and this
         * test would pass even with the routing bug present. */
        emit(frame(MC_OP.INPUT_EVENT, new Uint8Array([3, 1, 0])));
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.SET_OUTPUT, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const events: unknown[] = [];
    client.onInputEvent(e => events.push(e));

    const result = await client.setOutput(3, true);
    expect(result.ok).toBe(true);
    expect(result.result).toBe(MC_RESULT.OK);
    expect(events).toEqual([
      { button: 3, pressType: 'long', actionSuppressed: false },
    ]);
  });

  /* A board going out of range must fail in-flight work immediately and by
   * name. Previously each request sat armed for the full REPLY_TIMEOUT_MS and
   * then rejected with a bare "timed out", which reads as a protocol fault and
   * — if the screen had moved on — landed as an unhandled rejection seconds
   * after the event. */
  test('an unexpected disconnect rejects in-flight requests immediately', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      }
      // COMMAND deliberately never answers, standing in for a board that
      // vanished mid-request.
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const pending = client.setOutput(1, true);
    transport.dropConnection();

    await expect(pending).rejects.toThrow(/disconnected/i);
    expect(client.isAuthenticated()).toBe(false);
  });

  /* The exact race that produced an unhandled rejection on a real board:
   * request() creates the reply waiter, then awaits send(). A disconnect
   * landing inside that await rejects a promise that has no handler yet,
   * because the caller only awaits it a microtask later. waitForReply()
   * attaches a no-op catch at creation to close the gap — the caller still
   * receives the rejection, as this asserts. */
  test('a disconnect during send still rejects the caller, not the runtime', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    // Drop the link from inside send(), i.e. while request() is awaiting it
    // and before it has returned the waiter to its caller.
    transport.beforeSend = () => transport.dropConnection();

    await expect(client.setOutput(2, true)).rejects.toThrow(/disconnected/i);
  });

  test('configRead reassembles out-of-order chunks by offset', async () => {
    const transport = new ScriptedTransport();
    const json = JSON.stringify(defaultConfig());
    const bytes = utf8Encode(json);
    const mid = Math.floor(bytes.length / 2);
    const chunkA = concatBytes(
      u16le(0),
      u16le(bytes.length),
      bytes.subarray(0, mid),
    );
    const chunkB = concatBytes(
      u16le(mid),
      u16le(bytes.length),
      bytes.subarray(mid),
    );
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.CONFIG && opcode === MC_OP.CONFIG_READ) {
        // Emit the second half first to prove reassembly is offset-based,
        // not arrival-order-based.
        emit(frame(MC_OP.CONFIG_CHUNK, chunkB));
        emit(frame(MC_OP.CONFIG_CHUNK, chunkA));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const config = await client.configRead();
    expect(config).toEqual(defaultConfig());
  });

  test('configWrite sends BEGIN, chunk(s), then COMMIT, and resolves on the commit result', async () => {
    const transport = new ScriptedTransport();
    const seenOps: number[] = [];
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.CONFIG) return;
      seenOps.push(opcode);
      if (opcode === MC_OP.CONFIG_WRITE_COMMIT) {
        emit(frame(MC_OP.CONFIG_WRITE_RESULT, new Uint8Array([MC_RESULT.OK])));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const result = await client.configWrite(defaultConfig());
    expect(result.ok).toBe(true);
    expect(seenOps[0]).toBe(MC_OP.CONFIG_WRITE_BEGIN);
    expect(seenOps[seenOps.length - 1]).toBe(MC_OP.CONFIG_WRITE_COMMIT);
  });

  test('keyList parses a multi-key response', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel === MC_CH.AUTH && opcode === MC_OP.KEY_LIST) {
        const label0 = utf8Encode('phone A');
        const label1 = utf8Encode('phone B');
        const body = concatBytes(
          new Uint8Array([2]),
          new Uint8Array([0, label0.length]),
          label0,
          new Uint8Array([2, label1.length]),
          label1,
        );
        emit(frame(MC_OP.KEY_LIST_RESULT, body));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const keys = await client.keyList();
    expect(keys).toEqual([
      { slot: 0, label: 'phone A' },
      { slot: 2, label: 'phone B' },
    ]);
  });

  test('getStatus parses lockState and the cheatcodeBackoff bit', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(
          frame(
            MC_OP.STATUS,
            statusBytes({ lockState: 2, cheatcodeBackoff: true }),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const status = await client.getStatus();
    expect(status.lockState).toBe(2);
    expect(status.cheatcodeBackoff).toBe(true);
  });

  test('lock and unlock send the right opcodes and parse the result', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.LOCK) {
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.LOCK, MC_RESULT.OK]),
          ),
        );
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.UNLOCK) {
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.UNLOCK, MC_RESULT.REJECTED]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    expect((await client.lock()).ok).toBe(true);
    const unlockResult = await client.unlock();
    expect(unlockResult.ok).toBe(false);
    expect(unlockResult.result).toBe(MC_RESULT.REJECTED);
  });

  test('lockGetConfig / lockSetConfig round-trip the wire byte layout', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.LOCK_GET_CONFIG
      ) {
        const resp = new Uint8Array(9);
        resp[0] = 1; // enabled
        resp[1] = 0x03; // PHONE | IGNITION_SWITCH
        resp[2] = 0xff; // no ignition-switch input
        resp[3] = 0x60;
        resp[4] = 0xea; // 60000
        resp[5] = 0x88;
        resp[6] = 0x13; // 5000
        resp[7] = 1; // cheatcode_set
        resp[8] = 6; // cheatcode_len
        emit(frame(MC_OP.LOCK_CONFIG, resp));
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.LOCK_SET_CONFIG
      ) {
        expect(Array.from(body)).toEqual([
          1, 0x01, 0xff, 0x60, 0xea, 0x88, 0x13,
        ]);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.LOCK_SET_CONFIG, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const cfg = await client.lockGetConfig();
    expect(cfg).toEqual({
      immobilizerEnabled: true,
      methodsMask: 0x03,
      ignitionSwitchInput: -1,
      autoLockGraceMs: 60000,
      cheatcodeWindowMs: 5000,
      cheatcodeSet: true,
      cheatcodeLen: 6,
    });

    const setResult = await client.lockSetConfig({
      immobilizerEnabled: true,
      methodsMask: 0x01,
      ignitionSwitchInput: -1,
      autoLockGraceMs: 60000,
      cheatcodeWindowMs: 5000,
    });
    expect(setResult.ok).toBe(true);
  });

  test('cheatcodeSet/Clear/Test encode length-prefixed button arrays', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.CHEATCODE_SET) {
        expect(Array.from(body)).toEqual([4, 0, 1, 2, 3]);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.CHEATCODE_SET, MC_RESULT.OK]),
          ),
        );
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.CHEATCODE_CLEAR
      ) {
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.CHEATCODE_CLEAR, MC_RESULT.REJECTED]),
          ),
        );
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.CHEATCODE_TEST) {
        expect(Array.from(body)).toEqual([4, 0, 1, 2, 3]);
        emit(
          frame(MC_OP.CHEATCODE_TEST_RESULT, new Uint8Array([MC_RESULT.OK, 1])),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    expect((await client.cheatcodeSet([0, 1, 2, 3])).ok).toBe(true);
    expect((await client.cheatcodeClear()).ok).toBe(false);
    const test = await client.cheatcodeTest([0, 1, 2, 3]);
    expect(test.ok).toBe(true);
    expect(test.match).toBe(true);
  });

  test('transferOwnership sends the opcode and parses the result', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.TRANSFER_OWNERSHIP
      ) {
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.TRANSFER_OWNERSHIP, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const result = await client.transferOwnership();
    expect(result.ok).toBe(true);
  });

  // --- Diagnostics (docs/PROTOCOL.md §12) ---

  test('getStatus parses batteryMv, outputFaultMask, and the lvCutoffActive bit', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(
          frame(
            MC_OP.STATUS,
            statusBytes({ batteryMv: 11500, lvCutoffActive: true }),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const status = await client.getStatus();
    expect(status.batteryMv).toBe(11500);
    expect(status.lvCutoffActive).toBe(true);
    expect(status.cheatcodeBackoff).toBe(false); // bits are independent
  });

  test('getDiagnostics parses per-channel current + fault (DIAG_GET/DIAG_RESULT)', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.DIAG_GET) {
        const resp = new Uint8Array(1 + OUTPUT_COUNT * 2 + OUTPUT_COUNT);
        resp[0] = MC_RESULT.OK;
        resp.set(u16le(777), 1 + 2 * 3); // channel 3 current_ma = 777
        resp[1 + OUTPUT_COUNT * 2 + 3] = 1; // channel 3 fault = OPEN_LOAD
        emit(frame(MC_OP.DIAG_RESULT, resp));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');
    const diag = await client.getDiagnostics();
    expect(diag.channels).toHaveLength(OUTPUT_COUNT);
    expect(diag.channels[3]).toEqual({ currentMa: 777, fault: 1 });
    expect(diag.channels[0]).toEqual({ currentMa: 0, fault: 0 });
  });

  test('diagGetConfig / diagSetConfig round-trip the wire byte layout', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.DIAG_GET_CONFIG
      ) {
        const resp = new Uint8Array(1 + OUTPUT_COUNT * 4 + 8);
        resp[0] = MC_RESULT.OK;
        let pos = 1;
        for (let c = 0; c < OUTPUT_COUNT; c++) {
          resp.set(u16le(50), pos);
          pos += 2;
          resp.set(u16le(15000), pos);
          pos += 2;
        }
        resp.set(u16le(11800), pos);
        pos += 2;
        resp.set(u16le(300), pos);
        pos += 2;
        resp.set(u16le(13800), pos);
        pos += 2;
        resp.set(u16le(300), pos);
        emit(frame(MC_OP.DIAG_CONFIG, resp));
      } else if (
        channel === MC_CH.COMMAND &&
        opcode === MC_OP.DIAG_SET_CONFIG
      ) {
        expect(body.length).toBe(OUTPUT_COUNT * 4 + 8);
        // First channel's open_load_ma/overcurrent_ma, and the trailing cutoff word.
        expect(Array.from(body.subarray(0, 4))).toEqual([
          0x64, 0x00, 0x88, 0x13,
        ]); // 100, 5000
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.DIAG_SET_CONFIG, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const cfg = await client.diagGetConfig();
    expect(cfg.channels).toHaveLength(OUTPUT_COUNT);
    expect(cfg.channels[0]).toEqual({ openLoadMa: 50, overcurrentMa: 15000 });
    expect(cfg.lvCutoffMv).toBe(11800);
    expect(cfg.lvCutoffHysteresisMv).toBe(300);
    expect(cfg.engineRunMv).toBe(13800);
    expect(cfg.engineRunHysteresisMv).toBe(300);

    const edited = defaultDiagConfig();
    edited.channels[0] = { openLoadMa: 100, overcurrentMa: 5000 };
    const setResult = await client.diagSetConfig(edited);
    expect(setResult.ok).toBe(true);
  });

  test('diagGetCalib / diagSetCalib round-trip the IEEE754 float wire layout', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.DIAG_GET_CALIB) {
        const resp = concatBytes(
          new Uint8Array([MC_RESULT.OK]),
          f32le(2.5),
          i16le(-7),
          f32le(1700),
          f32le(11.5),
          i16le(3),
        );
        emit(frame(MC_OP.DIAG_CALIB, resp));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.DIAG_SET_CALIB) {
        expect(body.length).toBe(16);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.DIAG_SET_CALIB, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const calib = await client.diagGetCalib();
    expect(calib.isGain).toBeCloseTo(2.5);
    expect(calib.isOffsetMv).toBe(-7);
    expect(calib.kilis).toBeCloseTo(1700);
    expect(calib.vbatGain).toBeCloseTo(11.5);
    expect(calib.vbatOffsetMv).toBe(3);

    const setResult = await client.diagSetCalib(defaultDiagCalib());
    expect(setResult.ok).toBe(true);
  });

  test('diagLearn sends the channel byte (including DIAG_LEARN_ALL) and parses the result', async () => {
    const transport = new ScriptedTransport();
    const seenChannels: number[] = [];
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.DIAG_LEARN) {
        seenChannels.push(body[0]!);
        const result = body[0] === 9 ? MC_RESULT.REJECTED : MC_RESULT.OK;
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.DIAG_LEARN, result]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    expect((await client.diagLearn(3)).ok).toBe(true);
    expect((await client.diagLearn(9)).ok).toBe(false);
    expect((await client.diagLearn(DIAG_LEARN_ALL)).ok).toBe(true);
    expect(seenChannels).toEqual([3, 9, DIAG_LEARN_ALL]);
  });

  test('hazardPress sends a payload-less HAZARD_PRESS and parses the result', async () => {
    const transport = new ScriptedTransport();
    let sawHazardPress = false;
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.HAZARD_PRESS) {
        sawHazardPress = true;
        expect(body.length).toBe(0);
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.HAZARD_PRESS, MC_RESULT.OK]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const result = await client.hazardPress();
    expect(sawHazardPress).toBe(true);
    expect(result.ok).toBe(true);
  });

  test('hazardPress reports REJECTED when the device has no turn channels configured', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.HAZARD_PRESS) {
        emit(
          frame(
            MC_OP.COMMAND_RESULT,
            new Uint8Array([MC_OP.HAZARD_PRESS, MC_RESULT.REJECTED]),
          ),
        );
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const result = await client.hazardPress();
    expect(result.ok).toBe(false);
  });

  // --- OTA (docs/PROTOCOL.md §10) ---

  test('uploadFirmware drives begin -> chunk* -> commit and reports chunk offsets/progress', async () => {
    const transport = new ScriptedTransport();
    const image = new Uint8Array(300).map((_, i) => i & 0xff);
    const sha512 = new Uint8Array(64).fill(0xab);
    const signature = new Uint8Array(64).fill(0xcd);
    const seenChunkOffsets: number[] = [];
    let seenBeginPayload: Uint8Array | null = null;

    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.OTA) return;
      if (opcode === MC_OP.OTA_BEGIN) {
        seenBeginPayload = body;
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
      } else if (opcode === MC_OP.OTA_CHUNK) {
        seenChunkOffsets.push(
          body[0]! | (body[1]! << 8) | (body[2]! << 16) | (body[3]! << 24),
        );
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
      } else if (opcode === MC_OP.OTA_COMMIT) {
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
      }
    };

    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const progress: Array<[number, number]> = [];
    const result = await client.uploadFirmware(
      { imageSize: image.length, sha512, signature, image },
      (sent, total) => progress.push([sent, total]),
    );

    expect(result.ok).toBe(true);
    expect(seenBeginPayload).not.toBeNull();
    expect(Array.from(seenBeginPayload!.subarray(0, 4))).toEqual(
      Array.from(u32le(image.length)),
    );
    expect(Array.from(seenBeginPayload!.subarray(4, 68))).toEqual(
      Array.from(sha512),
    );
    expect(Array.from(seenBeginPayload!.subarray(68, 132))).toEqual(
      Array.from(signature),
    );
    // 300 bytes at OTA_CHUNK_BYTES=500 fits in a single chunk at offset 0.
    expect(seenChunkOffsets).toEqual([0]);
    expect(progress).toEqual([[300, 300]]);
  });

  test('uploadFirmware stops at the first rejected chunk without committing', async () => {
    const transport = new ScriptedTransport();
    const image = new Uint8Array(1200); // spans 3 chunks at OTA_CHUNK_BYTES=500
    let sawCommit = false;
    let chunkCount = 0;

    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.OTA) return;
      if (opcode === MC_OP.OTA_BEGIN) {
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
      } else if (opcode === MC_OP.OTA_CHUNK) {
        chunkCount++;
        const result = chunkCount === 2 ? MC_RESULT.BAD_REQUEST : MC_RESULT.OK;
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([result])));
      } else if (opcode === MC_OP.OTA_COMMIT) {
        sawCommit = true;
        emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
      }
    };

    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const result = await client.uploadFirmware({
      imageSize: image.length,
      sha512: new Uint8Array(64),
      signature: new Uint8Array(64),
      image,
    });

    expect(result.ok).toBe(false);
    expect(result.result).toBe(MC_RESULT.BAD_REQUEST);
    expect(chunkCount).toBe(2); // stopped after the second (rejected) chunk
    expect(sawCommit).toBe(false);
  });

  test('otaStatus parses state/bytesReceived/imageSize', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.OTA && opcode === MC_OP.OTA_STATUS) {
        const resp = concatBytes(
          new Uint8Array([MC_RESULT.OK, OTA_STATE.COMMITTED]),
          u32le(2048),
          u32le(2048),
        );
        emit(frame(MC_OP.OTA_STATUS_RESULT, resp));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const status = await client.otaStatus();
    expect(status.ok).toBe(true);
    expect(status.state).toBe(OTA_STATE.COMMITTED);
    expect(status.bytesReceived).toBe(2048);
    expect(status.imageSize).toBe(2048);
  });

  test('otaAbort and otaReboot send payload-less requests and parse the result', async () => {
    const transport = new ScriptedTransport();
    const seenOps: number[] = [];
    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.OTA) return;
      seenOps.push(opcode);
      expect(body.length).toBe(0);
      emit(frame(MC_OP.OTA_RESULT, new Uint8Array([MC_RESULT.OK])));
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    expect((await client.otaAbort()).ok).toBe(true);
    expect((await client.otaReboot()).ok).toBe(true);
    expect(seenOps).toEqual([MC_OP.OTA_ABORT, MC_OP.OTA_REBOOT]);
  });

  // --- Event log (docs/PROTOCOL.md §15) ---

  test('getEventLog reassembles multiple EVENT_LOG_CHUNK frames into ordered records', async () => {
    const transport = new ScriptedTransport();
    const record = (
      seq: number,
      uptimeMs: number,
      type: number,
      arg0: number,
    ) =>
      concatBytes(
        u32le(seq),
        u32le(uptimeMs),
        new Uint8Array([type, arg0, 0, 0]),
      );

    transport.responder = (channel, opcode, body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
        return;
      }
      if (channel !== MC_CH.COMMAND || opcode !== MC_OP.EVENT_LOG_GET) return;
      expect(Array.from(body)).toEqual(Array.from(u32le(0)));
      // Two chunks: total=3 records, 2 in the first frame, 1 in the second.
      const chunk1 = concatBytes(
        u16le(0),
        u16le(3),
        new Uint8Array([2]),
        record(1, 100, EVENT_TYPE.KEY_ENROLLED, 0),
        record(2, 200, EVENT_TYPE.LOCK_ENGAGED, 0),
      );
      const chunk2 = concatBytes(
        u16le(2),
        u16le(3),
        new Uint8Array([1]),
        record(3, 300, EVENT_TYPE.KEY_REVOKED, 0),
      );
      emit(frame(MC_OP.EVENT_LOG_CHUNK, chunk1));
      emit(frame(MC_OP.EVENT_LOG_CHUNK, chunk2));
    };

    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const records = await client.getEventLog(0);
    expect(records).toEqual([
      {
        seq: 1,
        uptimeMs: 100,
        type: EVENT_TYPE.KEY_ENROLLED,
        arg0: 0,
        arg1: 0,
      },
      {
        seq: 2,
        uptimeMs: 200,
        type: EVENT_TYPE.LOCK_ENGAGED,
        arg0: 0,
        arg1: 0,
      },
      { seq: 3, uptimeMs: 300, type: EVENT_TYPE.KEY_REVOKED, arg0: 0, arg1: 0 },
    ]);
  });

  test('getEventLog returns an empty array for an empty/zero-count log', async () => {
    const transport = new ScriptedTransport();
    transport.responder = (channel, opcode, _body, emit) => {
      if (channel === MC_CH.STATUS && opcode === MC_OP.STATUS_GET) {
        emit(frame(MC_OP.STATUS, statusBytes()));
      } else if (channel === MC_CH.COMMAND && opcode === MC_OP.EVENT_LOG_GET) {
        emit(frame(MC_OP.EVENT_LOG_CHUNK, new Uint8Array([0, 0, 0, 0, 0])));
      }
    };
    const client = new MotoClient(transport);
    activeClients.push(client);
    await client.connect('fake');

    const records = await client.getEventLog();
    expect(records).toEqual([]);
  });
});
