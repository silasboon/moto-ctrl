/**
 * The single owner of "are we connected to our board, and if not, are we
 * trying to be" — the piece that lets phone-as-key unlock the bike without
 * the app being open. Before this existed, reconnecting to a paired board
 * only ever happened from PairingScreen's own scan-and-match effect, which
 * only runs while that screen is mounted — i.e. only while the app is open
 * and the rider hasn't already paired. See src/ble/bleManager.ts for the
 * iOS state-restoration half of this, and
 * android/app/src/main/java/com/motoctrl/app/ble/BleWatchService.kt for the
 * Android foreground-service half — both exist because a phone that has
 * frozen or killed this app's process can't run any of the JS below.
 *
 * State machine:
 *
 *   idle --(known device found)--> watching --(radio on, device reachable)-->
 *   connecting --(GATT up)--> authenticating --(challenge-response ok)-->
 *   connected --(disconnect)--> watching (retry)
 *
 * connecting/authenticating failing (not just "device not in range yet",
 * which the underlying connectToDevice call simply keeps waiting through —
 * see BlePlxTransport.connect()'s use of it) drops to `error` and retries
 * after CONNECT_RETRY_MS or AUTH_ERROR_RETRY_MS (below), cycling back
 * through watching.
 *
 * connectManually() is the interactive "rider tapped a board in
 * PairingScreen's list" path — a genuinely new board (trust-on-first-use),
 * or an impatient manual reconnect of the known one rather than waiting for
 * the watcher's own timing. It runs through the exact same connect+auth
 * logic and the exact same post-connect bookkeeping (disconnect handling,
 * event emission) as the automatic path, so a session behaves identically
 * afterward no matter how it started — and it shares establishSession's
 * in-flight guard with the watcher, so a tap on the board the watcher is
 * already mid-connecting-to joins that same attempt rather than racing it
 * with a second one.
 */
import { Platform } from 'react-native';
import { State } from 'react-native-ble-plx';

import type { Identity, LastDevice } from '../identity/KeyStore';
import {
  loadLastDevice,
  loadOrCreateIdentity,
  saveLastDevice,
} from '../identity/KeyStore';
import { MotoClient } from '../protocol/MotoClient';
import { BlePlxTransport } from '../transport/BlePlxTransport';
import type { DeviceDescriptor } from '../transport/Transport';
import { getBleManager } from './bleManager';
import { ensureWatchServiceRunning, stopWatchService } from './nativeBleWatchService';

/** Real BLE errors are rare and usually transient (radio hiccup, GATT
 * timeout) — a short retry keeps reconnect feeling responsive. Auth
 * rejections are rarer still and usually mean something a short retry won't
 * fix (this phone's key was revoked, the board has other owners) — a longer
 * one avoids spinning the radio for no benefit. Distinguished by
 * AuthRejectedError below, thrown only for the two rejection cases inside
 * establishSession — everything else (client.connect() failing, a thrown
 * plain Error) is treated as the transient case. */
const CONNECT_RETRY_MS = 5000;
const AUTH_ERROR_RETRY_MS = 30000;

class AuthRejectedError extends Error {}

export type BoardSessionState =
  /** No known device yet (fresh install, or after ownership transfer/wipe
   * of THIS phone's own KeyStore — see docs/TESTING.md). */
  | { type: 'idle' }
  /** Waiting on the radio and/or the device to become reachable. `reason`
   * is for UI copy only — same watching behaviour either way. */
  | { type: 'watching'; reason: 'initial' | 'lost' | 'retry' }
  | { type: 'connecting' }
  | { type: 'authenticating' }
  | { type: 'connected'; client: MotoClient; device: DeviceDescriptor }
  /** Connect or auth failed outright (not "not in range yet" — see the
   * module doc comment). Will retry on its own; `message` is surfaced so a
   * screen CAN show it, not because anyone has to act on it. */
  | { type: 'error'; message: string };

let state: BoardSessionState = { type: 'idle' };
const listeners = new Set<(s: BoardSessionState) => void>();

let started = false;
/** Set by stop(); checked by the post-connect disconnect handler so an
 * explicit rider Disconnect doesn't immediately resume watching and
 * silently reconnect out from under them. */
let stopRequested = false;
let radioSub: { remove(): void } | null = null;
let retryTimer: ReturnType<typeof setTimeout> | null = null;
let watchingFor: LastDevice | null = null;

/** Keyed so a tap on the device the watcher is already mid-connecting-to
 * joins that attempt instead of starting a second, redundant one. */
let inFlight: { deviceId: string; promise: Promise<MotoClient> } | null = null;

function setState(next: BoardSessionState): void {
  state = next;
  for (const listener of listeners) listener(next);
  if (Platform.OS === 'android') {
    updateAndroidNotification(next);
  }
}

function updateAndroidNotification(s: BoardSessionState): void {
  switch (s.type) {
    case 'idle':
      stopWatchService();
      return;
    case 'watching':
      ensureWatchServiceRunning('Watching for your board…');
      return;
    case 'connecting':
    case 'authenticating':
      ensureWatchServiceRunning('Connecting to your board…');
      return;
    case 'connected':
      ensureWatchServiceRunning(`Connected to ${s.device.name}`);
      return;
    case 'error':
      ensureWatchServiceRunning('Reconnecting to your board…');
      return;
  }
}

export function getState(): BoardSessionState {
  return state;
}

/** `listener` is called once immediately with the current state (so a
 * screen mounting after a connection already happened — e.g. restored from
 * an iOS background relaunch — sees it right away) and again on every
 * change. */
export function onStateChange(
  listener: (s: BoardSessionState) => void,
): () => void {
  listeners.add(listener);
  listener(state);
  return () => listeners.delete(listener);
}

function clearRetryTimer(): void {
  if (retryTimer) {
    clearTimeout(retryTimer);
    retryTimer = null;
  }
}

/** Connects and runs the full challenge-response (enrolling via
 * trust-on-first-use if this phone isn't recognised yet — safe to attempt
 * unconditionally here because the firmware itself only ever accepts TOFU
 * enrollment on a board with no keys enrolled at all, docs/PROTOCOL.md §6;
 * a phone whose key was deliberately revoked cannot re-enroll itself this
 * way). Persists the device as "last paired" on success. Throws on any
 * failure; never leaves a half-open connection behind. */
async function establishSession(
  device: DeviceDescriptor,
  identity: Identity,
): Promise<MotoClient> {
  const existing = inFlight;
  if (existing && existing.deviceId === device.id) {
    return existing.promise;
  }

  const attempt = (async (): Promise<MotoClient> => {
    const client = new MotoClient(new BlePlxTransport());
    try {
      await client.connect(device.id);
      let auth = await client.authenticate(identity.keypair);
      if (!auth.ok) {
        const enrolled = await client.enroll(
          identity.keypair.publicKey,
          identity.label,
        );
        if (!enrolled.ok) {
          throw new AuthRejectedError(
            `This board already has paired phones (${enrolled.resultName}). ` +
              'Ask a paired phone to add this one from its Paired Keys screen.',
          );
        }
        auth = await client.authenticate(identity.keypair);
        if (!auth.ok) {
          throw new AuthRejectedError(
            `Authentication failed after pairing (${auth.resultName}).`,
          );
        }
      }
      await saveLastDevice({ id: device.id, name: device.name });
      return client;
    } catch (err) {
      await client.disconnect().catch(() => {});
      throw err;
    }
  })();

  inFlight = { deviceId: device.id, promise: attempt };
  try {
    return await attempt;
  } finally {
    if (inFlight?.promise === attempt) inFlight = null;
  }
}

/** Wires a connected session into the shared state and arms the
 * disconnect handler that resumes watching (unless stop() was called). */
function adoptSession(client: MotoClient, device: DeviceDescriptor): void {
  setState({ type: 'connected', client, device });
  client.onConnectionStateChange(connState => {
    if (connState !== 'disconnected') return;
    if (stopRequested) return;
    watchFor({ id: device.id, name: device.name }, 'lost');
  });
}

/** One connect+auth attempt against a known, presumed-reachable device.
 * Called both by the watcher (device.id already reachable, or the call
 * simply waits until it is — see connectToDevice's own semantics) and by
 * connectManually. */
async function attemptConnect(device: DeviceDescriptor): Promise<void> {
  setState({ type: 'connecting' });
  try {
    const identity = await loadOrCreateIdentity();
    setState({ type: 'authenticating' });
    const client = await establishSession(device, identity);
    adoptSession(client, device);
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err);
    setState({ type: 'error', message });
    clearRetryTimer();
    const delay = err instanceof AuthRejectedError ? AUTH_ERROR_RETRY_MS : CONNECT_RETRY_MS;
    retryTimer = setTimeout(() => {
      watchFor({ id: device.id, name: device.name }, 'retry');
    }, delay);
  }
}

/** Waits for the radio, then attempts a connection. Re-entrant-safe: tears
 * down any previous radio subscription first, so calling this again (a
 * disconnect, a retry, or watching for a newly-paired device) never leaves
 * two subscriptions racing each other. */
function watchFor(known: LastDevice, reason: 'initial' | 'lost' | 'retry'): void {
  radioSub?.remove();
  radioSub = null;
  clearRetryTimer();
  watchingFor = known;
  setState({ type: 'watching', reason });

  const manager = getBleManager();
  const sub = manager.onStateChange(radioState => {
    if (radioState !== State.PoweredOn) return;
    sub.remove();
    if (radioSub === sub) radioSub = null;
    void attemptConnect({ id: known.id, name: known.name });
  }, true);
  radioSub = sub;
}

/** Begins (or resumes) watching for the known device — idempotent, safe to
 * call repeatedly (index.js calls it unconditionally at boot; PairingScreen
 * calls it again on mount so a rider returning from an explicit Disconnect
 * gets reconnect back without relaunching the app). No-op if there is no
 * known device yet, or if already running. */
export function start(): void {
  if (started) return;
  started = true;
  stopRequested = false;
  void (async () => {
    const known = await loadLastDevice();
    if (!known) {
      setState({ type: 'idle' });
      return;
    }
    watchFor(known, 'initial');
  })();
}

/** Explicit rider "Disconnect": stops watching and drops any live
 * connection, and does NOT resume on its own — start() (called again by
 * PairingScreen's mount) re-arms it. */
export async function stop(): Promise<void> {
  started = false;
  stopRequested = true;
  radioSub?.remove();
  radioSub = null;
  clearRetryTimer();
  watchingFor = null;
  const s = state;
  setState({ type: 'idle' });
  if (s.type === 'connected') {
    await s.client.disconnect().catch(() => {});
  }
}

/** The interactive path: a rider tapped a board in PairingScreen's list —
 * a new board (trust-on-first-use) or a manual reconnect of the known one.
 * Always supersedes any existing watch first (even for the same device —
 * a stale watcher subscription must never be left to also react once this
 * attempt has already decided the outcome), then runs the same connect+auth
 * establishSession() the automatic watcher uses.
 *
 * On failure, resumes watching whatever was being watched before this call
 * (if anything) rather than leaving the shared state stuck mid-attempt —
 * this is exception-safe specifically so a failed manual tap can never
 * strand BoardSession in 'authenticating' forever with nothing watching. */
export async function connectManually(
  device: DeviceDescriptor,
  identity: Identity,
): Promise<MotoClient> {
  /* Set immediately, not just on success: a concurrent start() (e.g.
   * PairingScreen remounting while this call is still in flight) checks
   * `started` as a guard against exactly this — running its own watchFor()
   * alongside an in-progress manual connect would leave two subscriptions
   * disagreeing about what's happening. */
  started = true;
  stopRequested = false;

  const previousWatch = watchingFor;
  radioSub?.remove();
  radioSub = null;
  clearRetryTimer();
  watchingFor = null;

  setState({ type: 'connecting' });
  setState({ type: 'authenticating' });
  try {
    const client = await establishSession(device, identity);
    adoptSession(client, device);
    return client;
  } catch (err) {
    if (previousWatch) {
      watchFor(previousWatch, 'retry');
    } else {
      setState({ type: 'idle' });
    }
    throw err;
  }
}
