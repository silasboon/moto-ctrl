/**
 * Scanning must not start until the radio is actually on.
 *
 * The bug this pins down: the app started a scan the instant it opened, while
 * CoreBluetooth was still reporting `Unknown`. startDeviceScan failed
 * immediately, the failure was swallowed as "no device found", and the board
 * was only ever discovered on the retry a rider tapped after the first search
 * timed out — by which point the radio had come up on its own.
 */
import { State } from 'react-native-ble-plx';

import { BlePlxTransport } from '../transport/BlePlxTransport';
import type { ScanStatus } from '../transport/Transport';

type StateListener = (state: State) => void;
type ScanListener = (error: unknown, device: unknown) => void;

const mockManager = {
  stateListener: null as StateListener | null,
  scanListener: null as ScanListener | null,
  startCalls: 0,
  stopCalls: 0,
  onStateChange: jest.fn(
    (listener: StateListener, emitCurrentState: boolean) => {
      mockManager.stateListener = listener;
      if (emitCurrentState) listener(mockManager.currentState);
      return { remove: jest.fn() };
    },
  ),
  startDeviceScan: jest.fn(
    (_uuids: unknown, _opts: unknown, listener: ScanListener) => {
      mockManager.startCalls += 1;
      mockManager.scanListener = listener;
    },
  ),
  stopDeviceScan: jest.fn(() => {
    mockManager.stopCalls += 1;
  }),
  currentState: State.Unknown as State,
};

jest.mock('react-native-ble-plx', () => {
  const actual = jest.requireActual('react-native-ble-plx');
  return {
    ...actual,
    BleManager: jest.fn(() => mockManager),
  };
});

function reset(state: State): void {
  mockManager.currentState = state;
  mockManager.stateListener = null;
  mockManager.scanListener = null;
  mockManager.startCalls = 0;
  mockManager.stopCalls = 0;
}

describe('BlePlxTransport.scan', () => {
  test('waits for the radio instead of scanning into a cold adapter', () => {
    reset(State.Unknown);
    const statuses: ScanStatus[] = [];
    const stop = new BlePlxTransport().scan(jest.fn(), s => statuses.push(s));

    expect(mockManager.startCalls).toBe(0);
    expect(statuses).toEqual([
      { state: 'waiting', message: 'Waiting for Bluetooth…' },
    ]);

    /* Radio comes up a moment later, as it always does on a cold start. */
    mockManager.stateListener?.(State.PoweredOn);
    expect(mockManager.startCalls).toBe(1);
    expect(statuses[statuses.length - 1]).toEqual({ state: 'scanning' });

    stop();
  });

  test('scans immediately when the radio is already on', () => {
    reset(State.PoweredOn);
    const stop = new BlePlxTransport().scan(jest.fn());
    expect(mockManager.startCalls).toBe(1);
    stop();
    expect(mockManager.stopCalls).toBe(1);
  });

  test('says Bluetooth is off rather than reporting no boards', () => {
    reset(State.PoweredOff);
    const statuses: ScanStatus[] = [];
    const stop = new BlePlxTransport().scan(jest.fn(), s => statuses.push(s));

    expect(mockManager.startCalls).toBe(0);
    expect(statuses[0]).toEqual({
      state: 'waiting',
      message: 'Bluetooth is off. Turn it on to find your board.',
    });
    stop();
  });

  test('reports a board advertising its name in either field', () => {
    reset(State.PoweredOn);
    const found: { id: string; name: string }[] = [];
    const stop = new BlePlxTransport().scan(d => found.push(d));

    mockManager.scanListener?.(null, { id: 'aa', name: 'MOTO-CTRL' });
    /* iOS can leave `name` null on a first sighting and carry the advertised
     * name in localName instead. */
    mockManager.scanListener?.(null, {
      id: 'bb',
      name: null,
      localName: 'MOTO-CTRL',
    });
    mockManager.scanListener?.(null, { id: 'cc', name: 'Some Headphones' });

    expect(found).toEqual([
      { id: 'aa', name: 'MOTO-CTRL' },
      { id: 'bb', name: 'MOTO-CTRL' },
    ]);
    stop();
  });

  test('drops the scan and explains when the radio goes away mid-scan', () => {
    reset(State.PoweredOn);
    const statuses: ScanStatus[] = [];
    const stop = new BlePlxTransport().scan(jest.fn(), s => statuses.push(s));
    expect(statuses[0]).toEqual({ state: 'scanning' });

    mockManager.stateListener?.(State.PoweredOff);
    expect(mockManager.stopCalls).toBe(1);
    expect(statuses[statuses.length - 1]).toEqual({
      state: 'waiting',
      message: 'Bluetooth is off. Turn it on to find your board.',
    });
    stop();
  });
});
