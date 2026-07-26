/**
 * Byte-level helpers shared by the protocol client and its transports.
 * Mirrors the equivalent helpers in firmware/sim/gui/app.js.
 *
 * Includes a small self-contained base64 codec rather than depending on
 * `btoa`/`atob` (not guaranteed present in every RN/Hermes version) or
 * pulling in a dependency for it — react-native-ble-plx exchanges
 * characteristic values as base64 strings, so BlePlxTransport needs this
 * regardless of what's globally available.
 */

export function concatBytes(...parts: Uint8Array[]): Uint8Array {
  const len = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(len);
  let o = 0;
  for (const p of parts) {
    out.set(p, o);
    o += p.length;
  }
  return out;
}

export function u16le(v: number): Uint8Array {
  return new Uint8Array([v & 0xff, (v >> 8) & 0xff]);
}

export function readU16le(b: Uint8Array, off: number): number {
  return b[off]! | (b[off + 1]! << 8);
}

export function readU32le(b: Uint8Array, off: number): number {
  return (b[off]! | (b[off + 1]! << 8) | (b[off + 2]! << 16) | (b[off + 3]! << 24)) >>> 0;
}

export function u32le(v: number): Uint8Array {
  return new Uint8Array([v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, (v >>> 24) & 0xff]);
}

export function i16le(v: number): Uint8Array {
  const u = v & 0xffff;
  return new Uint8Array([u & 0xff, (u >> 8) & 0xff]);
}

export function readI16le(b: Uint8Array, off: number): number {
  const u = readU16le(b, off);
  return u > 0x7fff ? u - 0x10000 : u;
}

/** IEEE754 binary32, little-endian — the wire format for
 * diagnostics calibration (docs/PROTOCOL.md §12). Uses DataView here,
 * unlike the hand-rolled integer helpers above: a correct IEEE754
 * encode/decode written by hand is a real correctness risk not worth
 * taking on for this rarely-used calibration path, and DataView/ArrayBuffer
 * have been reliably supported in Hermes for years — unlike the
 * historically-flaky global btoa/atob that are the actual reason
 * bytesToBase64/base64ToBytes above are hand-rolled instead. */
export function f32le(v: number): Uint8Array {
  const buf = new ArrayBuffer(4);
  new DataView(buf).setFloat32(0, v, true);
  return new Uint8Array(buf);
}

export function readF32le(b: Uint8Array, off: number): number {
  const dv = new DataView(b.buffer, b.byteOffset + off, 4);
  return dv.getFloat32(0, true);
}

const B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

export function bytesToBase64(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i]!;
    const b1 = i + 1 < bytes.length ? bytes[i + 1]! : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2]! : 0;
    const triple = (b0 << 16) | (b1 << 8) | b2;
    out += B64_CHARS[(triple >> 18) & 0x3f];
    out += B64_CHARS[(triple >> 12) & 0x3f];
    out += i + 1 < bytes.length ? B64_CHARS[(triple >> 6) & 0x3f] : '=';
    out += i + 2 < bytes.length ? B64_CHARS[triple & 0x3f] : '=';
  }
  return out;
}

export function base64ToBytes(b64: string): Uint8Array {
  const clean = b64.replace(/[^A-Za-z0-9+/]/g, '');
  const out: number[] = [];
  for (let i = 0; i < clean.length; i += 4) {
    const c0 = B64_CHARS.indexOf(clean[i]!);
    const c1 = B64_CHARS.indexOf(clean[i + 1] ?? 'A');
    const c2 = clean[i + 2] !== undefined ? B64_CHARS.indexOf(clean[i + 2]!) : -1;
    const c3 = clean[i + 3] !== undefined ? B64_CHARS.indexOf(clean[i + 3]!) : -1;
    const triple = ((c0 < 0 ? 0 : c0) << 18) | ((c1 < 0 ? 0 : c1) << 12) | ((c2 < 0 ? 0 : c2) << 6) | (c3 < 0 ? 0 : c3);
    out.push((triple >> 16) & 0xff);
    if (c2 >= 0) out.push((triple >> 8) & 0xff);
    if (c3 >= 0) out.push(triple & 0xff);
  }
  return new Uint8Array(out);
}

export function utf8Encode(s: string): Uint8Array {
  return new TextEncoder().encode(s);
}

export function utf8Decode(bytes: Uint8Array): string {
  return new TextDecoder().decode(bytes);
}

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, '0'))
    .join('');
}
