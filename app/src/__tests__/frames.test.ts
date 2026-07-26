import { base64ToBytes, bytesToBase64, concatBytes, readU16le, readU32le, u16le, u32le, utf8Decode, utf8Encode } from '../protocol/frames';

describe('byte helpers', () => {
  test('u16le / readU16le round-trip', () => {
    for (const v of [0, 1, 255, 256, 4096, 65535]) {
      expect(readU16le(u16le(v), 0)).toBe(v);
    }
  });

  test('readU32le reads little-endian', () => {
    const bytes = new Uint8Array([0x78, 0x56, 0x34, 0x12]);
    expect(readU32le(bytes, 0)).toBe(0x12345678);
  });

  test('u32le / readU32le round-trip, including values needing the sign bit (bit 31)', () => {
    for (const v of [0, 1, 65535, 0x12345678, 0x80000000, 0xffffffff]) {
      expect(readU32le(u32le(v), 0)).toBe(v);
    }
  });

  test('concatBytes joins in order', () => {
    const out = concatBytes(new Uint8Array([1, 2]), new Uint8Array([]), new Uint8Array([3]));
    expect(Array.from(out)).toEqual([1, 2, 3]);
  });

  test('base64 round-trips arbitrary bytes, including lengths not divisible by 3', () => {
    for (const len of [0, 1, 2, 3, 4, 5, 16, 32, 64]) {
      const bytes = new Uint8Array(len).map((_, i) => (i * 37 + 7) % 256);
      expect(Array.from(base64ToBytes(bytesToBase64(bytes)))).toEqual(Array.from(bytes));
    }
  });

  test('utf8 encode/decode round-trips', () => {
    const s = 'Low Beam — 🏍';
    expect(utf8Decode(utf8Encode(s))).toBe(s);
  });
});
