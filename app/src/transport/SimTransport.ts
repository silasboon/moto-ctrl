/**
 * Transport implementation that talks to firmware/sim over a local
 * WebSocket connection, so the app can be developed and CI-tested against
 * the host simulator with no hardware attached. See docs/PROTOCOL.md §1:
 * each WebSocket message is [channel byte][opcode][payload].
 */
import type {
  RNWebSocket,
  RNWebSocketMessageEvent,
} from '../platform/webSocketGlobal';
import { WebSocketImpl } from '../platform/webSocketGlobal';
import type { ConnectionState, DeviceDescriptor, Transport } from './Transport';

export class SimTransport implements Transport {
  private ws: RNWebSocket | null = null;
  private state: ConnectionState = 'disconnected';
  private readonly stateListeners = new Set<(state: ConnectionState) => void>();
  private readonly messageListeners = new Set<
    (channel: number, data: Uint8Array) => void
  >();

  constructor(private readonly url: string) {}

  scan(onFound: (device: DeviceDescriptor) => void): () => void {
    // The sim is one fixed address, not something to discover over the air.
    // Report it immediately so a device-list UI has something to select,
    // consistent with how a real BLE scan would eventually report a device.
    onFound({ id: this.url, name: `sim: ${this.url}` });
    return () => {};
  }

  connect(deviceId: string): Promise<void> {
    if (this.ws) {
      return Promise.resolve();
    }
    const url = deviceId || this.url;
    return new Promise((resolve, reject) => {
      this.setState('connecting');
      let settled = false;
      const ws = new WebSocketImpl(url);
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => {
        this.ws = ws;
        settled = true;
        this.setState('connected');
        resolve();
      };
      ws.onmessage = (ev: RNWebSocketMessageEvent) => {
        const data = ev.data;
        if (!(data instanceof ArrayBuffer)) {
          return;
        }
        const bytes = new Uint8Array(data);
        if (bytes.length < 1) {
          return;
        }
        const channel = bytes[0]!;
        const payload = bytes.subarray(1);
        for (const listener of this.messageListeners) {
          listener(channel, payload);
        }
      };
      ws.onerror = () => {
        if (!settled) {
          settled = true;
          reject(new Error(`SimTransport: failed to connect to ${url}`));
        }
      };
      ws.onclose = () => {
        this.ws = null;
        this.setState('disconnected');
        if (!settled) {
          settled = true;
          reject(
            new Error(
              `SimTransport: connection closed before opening (${url})`,
            ),
          );
        }
      };
    });
  }

  disconnect(): Promise<void> {
    this.ws?.close();
    this.ws = null;
    this.setState('disconnected');
    return Promise.resolve();
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

  send(channel: number, data: Uint8Array): Promise<void> {
    if (!this.ws) {
      return Promise.reject(new Error('SimTransport: not connected'));
    }
    const frame = new Uint8Array(data.length + 1);
    frame[0] = channel;
    frame.set(data, 1);
    this.ws.send(frame.buffer);
    return Promise.resolve();
  }

  onMessage(listener: (channel: number, data: Uint8Array) => void): () => void {
    this.messageListeners.add(listener);
    return () => this.messageListeners.delete(listener);
  }

  private setState(state: ConnectionState): void {
    this.state = state;
    for (const listener of this.stateListeners) {
      listener(state);
    }
  }
}
