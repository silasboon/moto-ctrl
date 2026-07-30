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
  type Subscription,
} from 'react-native-ble-plx';

import { CHANNEL_GATT, DEVICE_NAME, MC_CH } from '../protocol/constants';
import { base64ToBytes, bytesToBase64 } from '../protocol/frames';
import type { ConnectionState, DeviceDescriptor, Transport } from './Transport';

const ALL_CHANNELS = [
  MC_CH.STATUS,
  MC_CH.AUTH,
  MC_CH.COMMAND,
  MC_CH.CONFIG,
  MC_CH.OTA,
];

export class BlePlxTransport implements Transport {
  private _manager: BleManager | null = null;
  private device: Device | null = null;
  private state: ConnectionState = 'disconnected';
  private readonly stateListeners = new Set<(state: ConnectionState) => void>();
  private readonly messageListeners = new Set<
    (channel: number, data: Uint8Array) => void
  >();
  private readonly monitors: Subscription[] = [];

  /** Lazy: constructing a BleManager touches the native BLE module
   * immediately (permissions/radio state on a real device; crashes outright
   * under Jest, which has none). Defer it to first actual use, so simply
   * instantiating a BlePlxTransport — e.g. while a user is on SimTransport —
   * has no side effects. */
  private get manager(): BleManager {
    if (!this._manager) {
      this._manager = new BleManager();
    }
    return this._manager;
  }

  scan(onFound: (device: DeviceDescriptor) => void): () => void {
    this.manager.startDeviceScan(
      null,
      null,
      (error: BleError | null, device: Device | null) => {
        if (error || !device) {
          return;
        }
        if (device.name === DEVICE_NAME) {
          onFound({ id: device.id, name: device.name });
        }
      },
    );
    return () => this.manager.stopDeviceScan();
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
