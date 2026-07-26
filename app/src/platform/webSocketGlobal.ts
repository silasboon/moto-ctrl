/**
 * A locally-typed handle to the runtime-global `WebSocket` React Native
 * provides. This project's tsconfig (@react-native/typescript-config)
 * deliberately excludes the "dom" lib, so the ambient `WebSocket` type that
 * *is* reachable here (pulled in transitively by tooling) is incomplete
 * (missing `binaryType`, event payload shapes, etc.) — rather than fight
 * that or globally augment a type we don't fully control, this file
 * captures the real runtime binding once, typed the way this codebase
 * actually uses it. `declare const WebSocket` below is compile-time only;
 * at runtime the bare identifier still resolves to the real global.
 */

export interface RNWebSocketMessageEvent {
  data: string | ArrayBuffer;
}

export interface RNWebSocketCloseEvent {
  code: number;
  reason: string;
}

export interface RNWebSocket {
  binaryType: 'arraybuffer' | 'blob';
  readonly readyState: number;
  onopen: (() => void) | null;
  onmessage: ((ev: RNWebSocketMessageEvent) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onclose: ((ev: RNWebSocketCloseEvent) => void) | null;
  send(data: string | ArrayBuffer): void;
  close(code?: number, reason?: string): void;
}

interface RNWebSocketCtor {
  new (url: string): RNWebSocket;
}

declare const WebSocket: RNWebSocketCtor;

export const WebSocketImpl: RNWebSocketCtor = WebSocket;
