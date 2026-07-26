/**
 * Firmware update check/download.
 *
 * This is the ONE exception to "the app talks to the board over BLE and
 * nothing else over the network" (AGENTS.md's "Exception: firmware update
 * check/download") — kept in its own module, deliberately separate from
 * protocol/ (which is BLE/simulator wire protocol only), so the one place
 * this app reaches outside BLE is easy to find and audit. Exactly two
 * anonymous HTTPS GETs, both to the single baked-in UPDATE_MANIFEST_URL
 * (constants.ts), and nothing else — no identifiers, no accounts, no other
 * host. See docs/PROTOCOL.md §10.5 for the manifest/.mcota bundle formats
 * parsed here, and tools/sign-firmware.py for how a release produces them.
 *
 * Callers must never let a failure here block BLE pairing/control of the
 * board (AGENTS.md's mandatory constraint) — catch UpdateCheckError and
 * show "unable to check for updates", don't propagate it as a fatal error.
 */
import nacl from 'tweetnacl';

import { MCOTA_HEADER_BYTES, MCOTA_MAGIC, UPDATE_MANIFEST_URL } from '../protocol/constants';
import { bytesToHex, readU32le, utf8Decode } from '../protocol/frames';
import type { FirmwareBundle, UpdateManifest } from '../protocol/types';

export class UpdateCheckError extends Error {}

/** Fetches the version manifest from the baked-in URL (or an override, for
 * tests). Throws UpdateCheckError on any network/shape problem — never
 * throws anything else. */
export async function fetchUpdateManifest(url: string = UPDATE_MANIFEST_URL): Promise<UpdateManifest> {
  let res: Response;
  try {
    res = await fetch(url);
  } catch (err) {
    throw new UpdateCheckError(`manifest fetch failed: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!res.ok) {
    throw new UpdateCheckError(`manifest fetch failed: HTTP ${res.status}`);
  }
  const json = (await res.json()) as Partial<UpdateManifest>;
  if (
    typeof json.version !== 'string' ||
    typeof json.bundle_url !== 'string' ||
    typeof json.bundle_sha512 !== 'string' ||
    typeof json.bundle_size !== 'number'
  ) {
    throw new UpdateCheckError('manifest is missing required fields');
  }
  return {
    version: json.version,
    changelog: typeof json.changelog === 'string' ? json.changelog : '',
    bundle_url: json.bundle_url,
    bundle_sha512: json.bundle_sha512,
    bundle_size: json.bundle_size,
  };
}

/** true if `remote` (e.g. a manifest's `version`) is newer than `current`
 * (e.g. STATUS's fwMajor.fwMinor.fwPatch, docs/PROTOCOL.md §5). A simple
 * "X.Y.Z" numeric comparison, not full semver (no prerelease/build
 * metadata) — deliberately as simple as the device's own version fields. */
export function isNewerVersion(current: string, remote: string): boolean {
  const parse = (v: string): [number, number, number] => {
    const parts = v.split('.').map((n) => parseInt(n, 10) || 0);
    return [parts[0] ?? 0, parts[1] ?? 0, parts[2] ?? 0];
  };
  const [cMaj, cMin, cPatch] = parse(current);
  const [rMaj, rMin, rPatch] = parse(remote);
  if (rMaj !== cMaj) return rMaj > cMaj;
  if (rMin !== cMin) return rMin > cMin;
  return rPatch > cPatch;
}

/** Downloads the `.mcota` bundle a manifest points at, checks it downloaded
 * intact (size + SHA-512 against the manifest's fields — transport-
 * integrity only; the real security boundary is the on-device Ed25519
 * signature check in mc_ota_begin(), docs/PROTOCOL.md §10.2, which this
 * does not and cannot replace), then parses it. */
export async function downloadFirmwareBundle(manifest: UpdateManifest): Promise<FirmwareBundle> {
  let res: Response;
  try {
    res = await fetch(manifest.bundle_url);
  } catch (err) {
    throw new UpdateCheckError(`bundle download failed: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!res.ok) {
    throw new UpdateCheckError(`bundle download failed: HTTP ${res.status}`);
  }
  const buf = new Uint8Array(await res.arrayBuffer());
  if (buf.length !== manifest.bundle_size) {
    throw new UpdateCheckError(`bundle size mismatch: manifest says ${manifest.bundle_size}, got ${buf.length}`);
  }
  const digest = bytesToHex(nacl.hash(buf)); // SHA-512
  if (digest !== manifest.bundle_sha512.toLowerCase()) {
    throw new UpdateCheckError('bundle sha512 does not match the manifest — download may be corrupt');
  }
  return parseMcotaBundle(buf);
}

/** Parses a `.mcota` bundle's header (docs/PROTOCOL.md §10.5): magic,
 * format version, image_size/sha512/signature (the exact fields
 * MotoClient.uploadFirmware() forwards to OTA_BEGIN/OTA_CHUNK). */
export function parseMcotaBundle(buf: Uint8Array): FirmwareBundle {
  if (buf.length < MCOTA_HEADER_BYTES) {
    throw new UpdateCheckError(`bundle too short (${buf.length} bytes) to contain a valid .mcota header`);
  }
  const magic = utf8Decode(buf.subarray(0, 4));
  if (magic !== MCOTA_MAGIC) {
    throw new UpdateCheckError(`bad .mcota magic: got "${magic}", expected "${MCOTA_MAGIC}"`);
  }
  const formatVersion = buf[4]!;
  if (formatVersion !== 1) {
    throw new UpdateCheckError(`unsupported .mcota format version: ${formatVersion}`);
  }
  const imageSize = readU32le(buf, 8);
  const sha512 = buf.subarray(12, 76);
  const signature = buf.subarray(76, 140);
  const image = buf.subarray(MCOTA_HEADER_BYTES);
  if (image.length !== imageSize) {
    throw new UpdateCheckError(
      `bundle image length (${image.length}) doesn't match its header's image_size (${imageSize})`,
    );
  }
  return { imageSize, sha512, signature, image };
}
