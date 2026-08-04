/**
 * Real-hardware Transport implementation, via react-native-ble-plx.
 *
 * UNVERIFIED: there is no board and no generated native project
 * (app/ios, app/android — see NATIVE_SETUP.md) to run this against yet.
 * This is a mechanical translation of docs/PROTOCOL.md §2's GATT layout
 * into ble-plx calls — it needs real bench verification once hardware
 * exists (see docs/HARDWARE_TESTING.md). Do not treat a clean
 * typecheck as evidence this works on a real board.
 *
 * iOS CoreBluetooth state restoration (`restoreStateIdentifier`, so
 * phone-as-key reconnects from a locked phone) and Android's
 * foreground-service reconnect strategy (AGENTS.md "Stack choices") are
 * NOT implemented here — that's background-reconnect hardening that's
 * actually load-bearing for the immobilizer (lock system). This transport
 * covers the foreground happy path: scan, connect, write, and be
 * notified — which is what the dashboard/pin mapper/pairing screens need.
 */
import {
  BleError,
  BleManager,
  type Characteristic,
  type Device,
  State,
  type Subscription,
} from 'react-native-ble-plx';

import {
  ADVERTISED_SERVICE_UUID,
  CHANNEL_GATT,
  DEVICE_NAME,
  MC_CH,
} from '../protocol/constants';
import { base64ToBytes, bytesToBase64 } from '../protocol/frames';
import type {
  ConnectionState,
  DeviceDescriptor,
  ScanStatus,
  Transport,
} from './Transport';

const ALL_CHANNELS = [
  MC_CH.STATUS,
  MC_CH.AUTH,
  MC_CH.COMMAND,
  MC_CH.CONFIG,
  MC_CH.OTA,
];

/** Rider-facing explanation for every radio state that isn't PoweredOn. */
function stateMessage(state: State): string {
  switch (state) {
    case State.PoweredOff:
      return 'Bluetooth is off. Turn it on to find your board.';
    case State.Unauthorized:
      return 'MOTO-CTRL is not allowed to use Bluetooth. Enable it in system settings.';
    case State.Unsupported:
      return 'This phone has no Bluetooth LE radio.';
    case State.Resetting:
      return 'Bluetooth is restarting…';
    default:
      return 'Waiting for Bluetooth…';
  }
}

/* Identifies a MOTO-CTRL board from its advertisement.
 *
 * Keyed on the advertised service UUID, not the name: boards are renameable
 * (schema_version 8), so a name match would find only boards nobody has
 * renamed — precisely the ones a rider is least likely to be looking for.
 * The firmware puts the UUID in the primary advertising payload for exactly
 * this reason, with the name in the scan response.
 *
 * The name check is kept as a fallback for the case where a scan result
 * arrives with no service data attached, which some Android stacks do on the
 * first sighting. It only ever finds unrenamed boards, so it is a safety net
 * and not the mechanism. */
function isMotoCtrl(device: Device): boolean {
  const advertised = device.serviceUUIDs ?? [];
  if (
    advertised.some(
      u => u.toLowerCase() === ADVERTISED_SERVICE_UUID.toLowerCase(),
    )
  ) {
    return true;
  }
  return device.name === DEVICE_NAME || device.localName === DEVICE_NAME;
}

/* One native manager for the whole app.
 *
 * Each BleManager is a separate CBCentralManager/BluetoothAdapter client with
 * its own startup delay and its own view of which peripherals exist. Handing
 * scanning and connecting their own instances — which is what a `new
 * BlePlxTransport()` per screen action used to do — meant paying that startup
 * race twice and connecting through a manager that had never seen the device
 * the other one found. Shared, both happen against the same radio session.
 *
 * Still lazy: constructing it touches the native module immediately
 * (permissions/radio state on a device; no native module at all under Jest),
 * so it must not happen just because a module was imported. */
let sharedManager: BleManager | null = null;

export class BlePlxTransport implements Transport {
  private device: Device | null = null;
  private state: ConnectionState = 'disconnected';
  private readonly stateListeners = new Set<(state: ConnectionState) => void>();
  private readonly messageListeners = new Set<
    (channel: number, data: Uint8Array) => void
  >();
  private readonly monitors: Subscription[] = [];

  private get manager(): BleManager {
    if (!sharedManager) {
      sharedManager = new BleManager();
    }
    return sharedManager;
  }

  /**
   * Scanning waits for the radio rather than assuming it.
   *
   * A scan started the moment the app opens used to find nothing: the manager
   * had only just been constructed, the adapter was still reporting `Unknown`,
   * startDeviceScan failed straight away, and the failure was swallowed by a
   * callback that treated any error as "no device". The screen then sat
   * "searching" for its whole timeout, and the retry a rider inevitably tapped
   * worked instantly — by then the radio was up. So: subscribe to the adapter
   * state first (emitCurrentState, so an already-on radio starts the scan on
   * the spot) and only scan once it is actually PoweredOn.
   */
  scan(
    onFound: (device: DeviceDescriptor) => void,
    onStatus?: (status: ScanStatus) => void,
  ): () => void {
    let stopped = false;
    let scanning = false;

    const startNativeScan = (): void => {
      if (stopped || scanning) return;
      scanning = true;
      onStatus?.({ state: 'scanning' });
      this.manager.startDeviceScan(
        null,
        null,
        (error: BleError | null, device: Device | null) => {
          if (error) {
            scanning = false;
            onStatus?.({ state: 'failed', message: error.message });
            return;
          }
          if (!device) return;
          if (!isMotoCtrl(device)) return;
          /* `name` is the GAP name; `localName` is what this particular
           * advertisement carried. Which of the two ble-plx populates
           * depends on the platform and on whether the peripheral has been
           * seen before, so prefer whichever is present. A board whose scan
           * response hasn't arrived yet still shows up under the factory
           * name rather than as a blank row. */
          onFound({
            id: device.id,
            name: device.localName ?? device.name ?? DEVICE_NAME,
          });
        },
      );
    };

    const stateSub: Subscription = this.manager.onStateChange(state => {
      if (stopped) return;
      if (state === State.PoweredOn) {
        startNativeScan();
        return;
      }
      /* Radio went away mid-scan (rider toggled Bluetooth, or airplane
       * mode) — drop the scan and say why. */
      if (scanning) {
        scanning = false;
        this.manager.stopDeviceScan();
      }
      onStatus?.({ state: 'waiting', message: stateMessage(state) });
    }, true);

    return () => {
      stopped = true;
      stateSub.remove();
      if (scanning) {
        scanning = false;
        this.manager.stopDeviceScan();
      }
    };
  }

  async connect(deviceId: string): Promise<void> {
    this.setState('connecting');
    try {
      const device = await this.manager.connectToDevice(deviceId);
      await device.discoverAllServicesAndCharacteristics();
      this.device = device;

      device.onDisconnected(() => {
        this.teardownMonitors();
        this.device = null;
        this.setState('disconnected');
      });

      for (const channel of ALL_CHANNELS) {
        const gatt = CHANNEL_GATT[channel];
        if (!gatt) {
          continue;
        }
        const sub = device.monitorCharacteristicForService(
          gatt.service,
          gatt.characteristic,
          (error: BleError | null, characteristic: Characteristic | null) => {
            if (error || !characteristic?.value) {
              return;
            }
            const bytes = base64ToBytes(characteristic.value);
            for (const listener of this.messageListeners) {
              listener(channel, bytes);
            }
          },
        );
        this.monitors.push(sub);
      }

      this.setState('connected');
    } catch (err) {
      this.setState('disconnected');
      throw err;
    }
  }

  async disconnect(): Promise<void> {
    this.teardownMonitors();
    if (this.device) {
      await this.manager.cancelDeviceConnection(this.device.id).catch(() => {});
    }
    this.device = null;
    this.setState('disconnected');
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

  async send(channel: number, data: Uint8Array): Promise<void> {
    const gatt = CHANNEL_GATT[channel];
    if (!this.device || !gatt) {
      throw new Error(
        `BlePlxTransport: not connected or unknown channel ${channel}`,
      );
    }
    await this.device.writeCharacteristicWithResponseForService(
      gatt.service,
      gatt.characteristic,
      bytesToBase64(data),
    );
  }

  onMessage(listener: (channel: number, data: Uint8Array) => void): () => void {
    this.messageListeners.add(listener);
    return () => this.messageListeners.delete(listener);
  }

  private teardownMonitors(): void {
    for (const sub of this.monitors.splice(0)) {
      sub.remove();
    }
  }

  private setState(state: ConnectionState): void {
    this.state = state;
    for (const listener of this.stateListeners) {
      listener(state);
    }
  }
}
