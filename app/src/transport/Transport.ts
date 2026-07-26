/**
 * Transport is the one boundary between app logic and however bytes
 * actually reach a MOTO-CTRL board. All BLE/key logic in this app must be
 * written against this interface, never against react-native-ble-plx or a
 * socket directly — see AGENTS.md ("Stack choices").
 *
 * Two implementations:
 *  - BlePlxTransport — real hardware, via react-native-ble-plx.
 *  - SimTransport     — firmware/sim over TCP/websocket, for local dev and CI
 *                        with no hardware attached.
 *
 * Channel-aware: docs/PROTOCOL.md §1 defines a "channel" as a
 * logical stream — one GATT characteristic on real BLE, the leading byte of
 * each frame on the WebSocket sim transport. Framing differs per transport
 * (BLE has no leading channel byte; the characteristic itself IS the
 * channel), so `send`/`onMessage` take the channel explicitly and each
 * transport implementation maps it to whatever its wire actually needs —
 * this keeps src/protocol/MotoClient.ts transport-agnostic.
 */

export type ConnectionState = 'disconnected' | 'connecting' | 'connected';

export interface DeviceDescriptor {
  /** Transport-specific identifier (BLE MAC/UUID, or sim connection id). Not trusted for auth — see AGENTS.md safety requirement 4. */
  id: string;
  name: string;
}

export interface Transport {
  scan(onFound: (device: DeviceDescriptor) => void): () => void;
  connect(deviceId: string): Promise<void>;
  disconnect(): Promise<void>;
  getConnectionState(): ConnectionState;
  onConnectionStateChange(listener: (state: ConnectionState) => void): () => void;

  /** Send an opcode+payload frame (no channel byte — the transport adds
   * whatever framing its wire needs) on the given logical channel. */
  send(channel: number, data: Uint8Array): Promise<void>;
  /** Subscribe to opcode+payload frames arriving on any channel. */
  onMessage(listener: (channel: number, data: Uint8Array) => void): () => void;
}
