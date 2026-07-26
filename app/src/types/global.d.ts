/**
 * Ambient declarations for runtime globals Hermes/React Native provides
 * that this project's restricted `lib` (no "dom") doesn't type. Only the
 * members this codebase actually uses.
 */
export {};

declare global {
  class TextEncoder {
    encode(input?: string): Uint8Array;
  }
  class TextDecoder {
    constructor(label?: string);
    decode(input?: Uint8Array): string;
  }
}
