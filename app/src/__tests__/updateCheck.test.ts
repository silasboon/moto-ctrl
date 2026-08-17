/**
 * Unit tests for the firmware update-check module — the one place this app
 * makes a network call outside BLE. Mocks global.fetch; no real network
 * access.
 */
import nacl from 'tweetnacl';

import { MCOTA_HEADER_BYTES } from '../protocol/constants';
import { bytesToHex, u32le, utf8Encode } from '../protocol/frames';
import {
  downloadFirmwareBundle,
  fetchUpdateManifest,
  isNewerVersion,
  parseMcotaBundle,
  UpdateCheckError,
} from '../update/updateCheck';

function buildMcota(
  image: Uint8Array,
  sha512?: Uint8Array,
  signature?: Uint8Array,
  magic = 'MCOT',
  formatVersion = 1,
): Uint8Array {
  const header = new Uint8Array(MCOTA_HEADER_BYTES);
  header.set(utf8Encode(magic).subarray(0, 4), 0);
  header[4] = formatVersion;
  header.set(u32le(image.length), 8);
  header.set(sha512 ?? new Uint8Array(64).fill(0xaa), 12);
  header.set(signature ?? new Uint8Array(64).fill(0xbb), 76);
  const out = new Uint8Array(MCOTA_HEADER_BYTES + image.length);
  out.set(header, 0);
  out.set(image, MCOTA_HEADER_BYTES);
  return out;
}

function mockFetchOnce(response: {
  ok: boolean;
  status?: number;
  json?: () => unknown;
  arrayBuffer?: () => ArrayBuffer;
}): void {
  (global as unknown as { fetch: jest.Mock }).fetch = jest
    .fn()
    .mockResolvedValue(response);
}

describe('parseMcotaBundle', () => {
  test('parses a well-formed bundle', () => {
    const image = new Uint8Array(64).map((_, i) => i);
    const sha512 = new Uint8Array(64).fill(0x11);
    const signature = new Uint8Array(64).fill(0x22);
    const bundle = parseMcotaBundle(buildMcota(image, sha512, signature));
    expect(bundle.imageSize).toBe(64);
    expect(Array.from(bundle.sha512)).toEqual(Array.from(sha512));
    expect(Array.from(bundle.signature)).toEqual(Array.from(signature));
    expect(Array.from(bundle.image)).toEqual(Array.from(image));
  });

  test('rejects a bad magic', () => {
    const image = new Uint8Array(8);
    expect(() =>
      parseMcotaBundle(buildMcota(image, undefined, undefined, 'XXXX')),
    ).toThrow(UpdateCheckError);
  });

  test('rejects an unsupported format version', () => {
    const image = new Uint8Array(8);
    expect(() =>
      parseMcotaBundle(buildMcota(image, undefined, undefined, 'MCOT', 99)),
    ).toThrow(UpdateCheckError);
  });

  test('rejects a truncated bundle (image shorter than declared image_size)', () => {
    const full = buildMcota(new Uint8Array(64));
    const truncated = full.subarray(0, MCOTA_HEADER_BYTES + 10);
    expect(() => parseMcotaBundle(truncated)).toThrow(UpdateCheckError);
  });

  test('rejects a buffer too short to hold a header', () => {
    expect(() => parseMcotaBundle(new Uint8Array(10))).toThrow(
      UpdateCheckError,
    );
  });
});

describe('isNewerVersion', () => {
  test.each([
    ['1.0.0', '1.0.1', true],
    ['1.0.0', '1.1.0', true],
    ['1.0.0', '2.0.0', true],
    ['1.2.3', '1.2.3', false],
    ['1.2.3', '1.2.2', false],
    ['2.0.0', '1.9.9', false],
  ])('isNewerVersion(%s, %s) === %s', (current, remote, expected) => {
    expect(isNewerVersion(current, remote)).toBe(expected);
  });
});

describe('fetchUpdateManifest', () => {
  afterEach(() => {
    jest.resetAllMocks();
  });

  test('parses a well-formed manifest', async () => {
    mockFetchOnce({
      ok: true,
      json: () => ({
        version: '1.2.0',
        changelog: 'Fixes things.',
        bundle_url: 'https://example.com/fw.mcota',
        bundle_sha512: 'ab'.repeat(64),
        bundle_size: 1000,
      }),
    });
    const manifest = await fetchUpdateManifest(
      'https://example.com/manifest.json',
    );
    expect(manifest.version).toBe('1.2.0');
    expect(manifest.bundle_size).toBe(1000);
  });

  test('throws UpdateCheckError on a non-OK HTTP response', async () => {
    mockFetchOnce({ ok: false, status: 404 });
    await expect(
      fetchUpdateManifest('https://example.com/manifest.json'),
    ).rejects.toThrow(UpdateCheckError);
  });

  test('throws UpdateCheckError when required fields are missing', async () => {
    mockFetchOnce({ ok: true, json: () => ({ version: '1.2.0' }) });
    await expect(
      fetchUpdateManifest('https://example.com/manifest.json'),
    ).rejects.toThrow(UpdateCheckError);
  });

  test('throws UpdateCheckError when the network call itself rejects', async () => {
    (global as unknown as { fetch: jest.Mock }).fetch = jest
      .fn()
      .mockRejectedValue(new Error('offline'));
    await expect(
      fetchUpdateManifest('https://example.com/manifest.json'),
    ).rejects.toThrow(UpdateCheckError);
  });
});

describe('downloadFirmwareBundle', () => {
  afterEach(() => {
    jest.resetAllMocks();
  });

  test('downloads, verifies sha512 + size against the manifest, and parses the bundle', async () => {
    const image = new Uint8Array(128).map((_, i) => (i * 3) & 0xff);
    const bundleBytes = buildMcota(image);
    const bundleSha512 = bytesToHex(nacl.hash(bundleBytes));

    mockFetchOnce({
      ok: true,
      arrayBuffer: () => bundleBytes.slice().buffer,
    });

    const bundle = await downloadFirmwareBundle(
      {
        version: '1.2.0',
        changelog: '',
        bundle_url: 'https://example.com/fw.mcota',
        bundle_sha512: bundleSha512,
        bundle_size: bundleBytes.length,
      },
      'example.com',
    );
    expect(bundle.imageSize).toBe(128);
    expect(Array.from(bundle.image)).toEqual(Array.from(image));
  });

  test('rejects a download whose sha512 does not match the manifest', async () => {
    const bundleBytes = buildMcota(new Uint8Array(32));
    mockFetchOnce({
      ok: true,
      arrayBuffer: () => bundleBytes.slice().buffer,
    });
    await expect(
      downloadFirmwareBundle(
        {
          version: '1.2.0',
          changelog: '',
          bundle_url: 'https://example.com/fw.mcota',
          bundle_sha512: 'ff'.repeat(64), // wrong on purpose
          bundle_size: bundleBytes.length,
        },
        'example.com',
      ),
    ).rejects.toThrow(UpdateCheckError);
  });

  test('rejects a download whose size does not match the manifest', async () => {
    const bundleBytes = buildMcota(new Uint8Array(32));
    mockFetchOnce({
      ok: true,
      arrayBuffer: () => bundleBytes.slice().buffer,
    });
    await expect(
      downloadFirmwareBundle(
        {
          version: '1.2.0',
          changelog: '',
          bundle_url: 'https://example.com/fw.mcota',
          bundle_sha512: bytesToHex(nacl.hash(bundleBytes)),
          bundle_size: bundleBytes.length + 1, // wrong on purpose
        },
        'example.com',
      ),
    ).rejects.toThrow(UpdateCheckError);
  });

  test('rejects a bundle_url on a different host than the manifest', async () => {
    await expect(
      downloadFirmwareBundle(
        {
          version: '1.2.0',
          changelog: '',
          bundle_url: 'https://evil.example/fw.mcota',
          bundle_sha512: 'ab'.repeat(64),
          bundle_size: 10,
        },
        'example.com',
      ),
    ).rejects.toThrow(UpdateCheckError);
  });

  test('rejects a non-https bundle_url even on the trusted host', async () => {
    await expect(
      downloadFirmwareBundle(
        {
          version: '1.2.0',
          changelog: '',
          bundle_url: 'http://example.com/fw.mcota',
          bundle_sha512: 'ab'.repeat(64),
          bundle_size: 10,
        },
        'example.com',
      ),
    ).rejects.toThrow(UpdateCheckError);
  });
});
