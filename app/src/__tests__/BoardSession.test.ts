/**
 * BoardSession's watch/connect/authenticate/retry state machine — the piece
 * that lets phone-as-key reconnect and unlock without the app being open
 * (App.tsx / index.js / PairingScreen.tsx all just subscribe to it).
 *
 * MotoClient and KeyStore are mocked so these tests exercise BoardSession's
 * own decisions (when to watch, when to retry, what a stop() does, that a
 * failed manual connect can't strand shared state) without needing a real
 * BLE stack underneath — that's BlePlxScan.test.ts's and MotoClient.test.ts's
 * job, one layer down. getBleManager is mocked at the src/ble/bleManager
 * seam specifically so these tests don't need to know react-native-ble-plx
 * exists at all.
 *
 * All mock-factory state below is deliberately `mock`-prefixed: Jest hoists
 * jest.mock() factories above the rest of the module, so a factory can only
 * close over variables Jest recognises as mocks by that naming convention
 * (see BlePlxScan.test.ts's `mockManager` for the existing precedent here).
 */
import {
  connectManually,
  getState,
  onStateChange,
  start,
  stop,
  type BoardSessionState,
} from '../ble/BoardSession';
import type { LastDevice } from '../identity/KeyStore';

type RadioListener = (state: string) => void;

const mockManager = {
  radioListener: null as RadioListener | null,
  removeCalls: 0,
  onStateChange: jest.fn((listener: RadioListener, emitCurrentState: boolean) => {
    mockManager.radioListener = listener;
    if (emitCurrentState) listener('Unknown');
    return {
      remove: jest.fn(() => {
        mockManager.removeCalls += 1;
        mockManager.radioListener = null;
      }),
    };
  }),
};

jest.mock('../ble/bleManager', () => ({
  getBleManager: () => mockManager,
}));

jest.mock(
  '../ble/nativeBleWatchService',
  () => ({
    ensureWatchServiceRunning: jest.fn(),
    stopWatchService: jest.fn(),
  }),
  { virtual: true },
);

const mockKnownDevice: LastDevice = { id: 'board-1', name: 'Test Board' };
const mockKeyStoreState = {
  lastDevice: mockKnownDevice as LastDevice | null,
  savedDevices: [] as LastDevice[],
};

jest.mock('../identity/KeyStore', () => ({
  loadLastDevice: jest.fn(async () => mockKeyStoreState.lastDevice),
  loadOrCreateIdentity: jest.fn(async () => ({
    keypair: { publicKey: new Uint8Array([1]), secretKey: new Uint8Array([2]) },
    label: 'Test Phone',
  })),
  saveLastDevice: jest.fn(async (d: LastDevice) => {
    mockKeyStoreState.savedDevices.push(d);
  }),
}));

/* One MotoClient class mock, reconfigured per test via mockClientState.
 * new MotoClient() reads it once per instance so different tests (and a
 * single test simulating retry-then-succeed) can vary the outcome. */
const mockClientState = {
  behavior: { authOk: true, enrollOk: true } as {
    authOk: boolean;
    enrollOk: boolean;
    connectError?: string;
  },
  constructedCount: 0,
  disconnectListeners: [] as Array<(s: string) => void>,
};

jest.mock('../protocol/MotoClient', () => {
  return {
    MotoClient: jest.fn().mockImplementation(() => {
      mockClientState.constructedCount += 1;
      const behavior = mockClientState.behavior;
      return {
        connect: jest.fn(async () => {
          if (behavior.connectError) throw new Error(behavior.connectError);
        }),
        authenticate: jest.fn(async () => ({
          ok: behavior.authOk,
          result: behavior.authOk ? 0 : 1,
          resultName: behavior.authOk ? 'OK' : 'UNAUTHENTICATED',
          slot: 0,
        })),
        enroll: jest.fn(async () => ({
          ok: behavior.enrollOk,
          result: behavior.enrollOk ? 0 : 1,
          resultName: behavior.enrollOk ? 'OK' : 'REJECTED',
        })),
        disconnect: jest.fn(async () => {}),
        onConnectionStateChange: jest.fn((listener: (s: string) => void) => {
          mockClientState.disconnectListeners.push(listener);
          return () => {};
        }),
      };
    }),
  };
});

function testIdentity() {
  return {
    keypair: { publicKey: new Uint8Array([1]), secretKey: new Uint8Array([2]) },
    label: 'Test Phone',
  };
}

async function flush(): Promise<void> {
  // Let queued microtasks (the async IIFEs inside start()/attemptConnect(),
  // each awaiting several mocked async calls in sequence: identity load,
  // connect, authenticate, [enroll, authenticate again], saveLastDevice)
  // actually run before assertions. More hops than strictly needed is
  // harmless -- resolving an already-settled promise is a no-op.
  for (let i = 0; i < 15; i++) {
    await Promise.resolve();
  }
}

beforeEach(async () => {
  await stop(); // resets module state between tests regardless of prior outcome
  mockManager.onStateChange.mockClear();
  mockManager.removeCalls = 0;
  mockManager.radioListener = null;
  mockKeyStoreState.lastDevice = mockKnownDevice;
  mockKeyStoreState.savedDevices.length = 0;
  mockClientState.constructedCount = 0;
  mockClientState.disconnectListeners.length = 0;
  mockClientState.behavior = { authOk: true, enrollOk: true };
});

test('start() with no known device settles on idle', async () => {
  mockKeyStoreState.lastDevice = null;
  start();
  await flush();
  expect(getState().type).toBe('idle');
});

test('start() with a known device watches, then connects once the radio is on', async () => {
  start();
  await flush();
  expect(getState().type).toBe('watching');

  mockManager.radioListener?.('PoweredOn');
  await flush();

  const s = getState();
  expect(s.type).toBe('connected');
  if (s.type === 'connected') {
    expect(s.device).toEqual(mockKnownDevice);
  }
  expect(mockClientState.constructedCount).toBe(1);
  expect(mockKeyStoreState.savedDevices).toEqual([mockKnownDevice]);
});

test('a disconnect while connected (not stopped) resumes watching', async () => {
  start();
  await flush();
  mockManager.radioListener?.('PoweredOn');
  await flush();
  expect(getState().type).toBe('connected');

  mockClientState.disconnectListeners[0]?.('disconnected');
  await flush();

  const s = getState();
  expect(s.type).toBe('watching');
  if (s.type === 'watching') {
    expect(s.reason).toBe('lost');
  }
});

test('stop() while connected disconnects and does not auto-resume', async () => {
  start();
  await flush();
  mockManager.radioListener?.('PoweredOn');
  await flush();
  expect(getState().type).toBe('connected');

  await stop();
  expect(getState().type).toBe('idle');

  // The disconnect stop() triggers must not resurrect watching on its own.
  mockClientState.disconnectListeners[0]?.('disconnected');
  await flush();
  expect(getState().type).toBe('idle');
});

test('connectManually() success reaches connected without duplicating the watcher', async () => {
  start();
  await flush();
  expect(getState().type).toBe('watching');

  const client = await connectManually(mockKnownDevice, testIdentity());
  expect(client).toBeTruthy();
  expect(getState().type).toBe('connected');
  // The pre-existing watch subscription must have been torn down, not left
  // running alongside the manual connection.
  expect(mockManager.removeCalls).toBeGreaterThanOrEqual(1);
});

test('connectManually() failure does not strand shared state mid-attempt', async () => {
  mockClientState.behavior = { authOk: false, enrollOk: false };
  await expect(
    connectManually(mockKnownDevice, testIdentity()),
  ).rejects.toThrow();
  // Must not be left at 'connecting'/'authenticating' forever -- back to
  // watching (there was nothing previously watched here) or idle.
  expect(['idle', 'watching']).toContain(getState().type);
});

test('two concurrent attempts at the same device share one MotoClient, not two', async () => {
  start();
  await flush();
  mockManager.radioListener?.('PoweredOn'); // watcher's own attemptConnect begins

  // A manual tap on the same device the watcher just started connecting to.
  const manual = connectManually(mockKnownDevice, testIdentity());
  await flush();
  await manual;

  expect(mockClientState.constructedCount).toBe(1);
});

test('state listeners receive the current state immediately on subscribe', async () => {
  mockKeyStoreState.lastDevice = null;
  start();
  await flush();

  const seen: BoardSessionState[] = [];
  const unsub = onStateChange(s => seen.push(s));
  unsub();
  expect(seen).toEqual([{ type: 'idle' }]);
});
