/**
 * Pairing: find a board over BLE and either authenticate (already enrolled)
 * or enroll this phone (first pairing / TOFU). See docs/PROTOCOL.md §6.
 *
 * Scanning starts on its own as soon as the screen appears, because with a
 * board on the bike there is exactly one thing a rider ever wants here.
 *
 * A known device is deliberately NOT auto-connected from this screen's own
 * scan results — src/ble/BoardSession.ts already does that, continuously,
 * independent of whether this screen (or the app at all) is open, which is
 * what lets phone-as-key reconnect and unlock without the app being open.
 * This screen calls BoardSession.start() on mount so returning here after an
 * explicit Disconnect resumes it, and its own scan/tap flow still exists for
 * pairing a genuinely new board or forcing an impatient manual reconnect —
 * both go through BoardSession.connectManually(), which shares its
 * in-flight/dedup guard with the watcher, so a tap on the board the watcher
 * is already mid-connecting-to joins that attempt rather than racing it.
 * App.tsx is what actually notices a connection happened (it subscribes to
 * BoardSession directly) and swaps this screen out — this screen never sees
 * the result of its own connect attempt beyond an error to show.
 *
 * The simulator transport is deliberately NOT offered here. SimTransport
 * still exists and is still required — the app's CI integration test drives
 * the real firmware/sim through it — but it is a development tool, not
 * something to put in front of a rider.
 */
import React, { useCallback, useEffect, useRef, useState } from 'react';
import {
  PermissionsAndroid,
  Platform,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';

import {
  connectManually,
  onStateChange as onBoardSessionStateChange,
  start as startBoardSession,
} from '../ble/BoardSession';
import {
  loadLastDevice,
  loadOrCreateIdentity,
  type Identity,
} from '../identity/KeyStore';
import { BlePlxTransport } from '../transport/BlePlxTransport';
import type { DeviceDescriptor } from '../transport/Transport';
import {
  Badge,
  Button,
  Card,
  EmptyState,
  Notice,
  Screen,
  SectionHeader,
} from '../ui/components';
import { colors, radius, space, type } from '../ui/theme';

/** How long to look before giving up — measured from when the radio is
 * actually scanning, not from the tap, since waiting for Bluetooth to come up
 * is not time spent looking. */
const SCAN_MS = 10000;

type PairingStatus =
  | 'idle'
  | 'waiting'
  | 'scanning'
  | 'connecting'
  | 'authenticating'
  | 'error';

interface Props {
  /** Shown once at the top — e.g. "the board disconnected" after a drop. */
  notice?: string | null;
}

export function PairingScreen({ notice }: Props): React.JSX.Element {
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [knownDeviceId, setKnownDeviceId] = useState<string | null>(null);
  const [found, setFound] = useState<DeviceDescriptor[]>([]);
  const [status, setStatus] = useState<PairingStatus>('idle');
  const [errorMsg, setErrorMsg] = useState<string | null>(null);
  /** Why the radio isn't scanning yet — "Bluetooth is off", and so on. */
  const [waitingMsg, setWaitingMsg] = useState<string | null>(null);

  const stopScanRef = useRef<(() => void) | null>(null);
  const scanTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** Guards a tap so a re-render or a second tap can't start a second
   * connect attempt for the same board. */
  const connectingRef = useRef(false);

  const busy = status === 'connecting' || status === 'authenticating';

  /* Mirrors BoardSession's own connecting/authenticating/error states, so
   * this screen shows live status for BOTH a manual tap and the automatic
   * background watcher — a known device failing to reconnect on its own is
   * now visible here too, not just silent. A manual tap's own try/catch
   * (below) still owns setting 'error' with a message tied to THIS
   * attempt specifically; this only upgrades status while nothing is
   * already reporting a more specific one from a direct tap. */
  useEffect(() => {
    return onBoardSessionStateChange(s => {
      if (connectingRef.current) return; // a direct tap owns status/errorMsg for its own duration
      if (s.type === 'connecting' || s.type === 'authenticating') {
        setStatus(s.type);
      } else if (s.type === 'error') {
        setStatus('error');
        setErrorMsg(s.message);
      }
    });
  }, []);

  /** Android needs runtime grants for BLUETOOTH_SCAN/CONNECT (API 31+) or
   * ACCESS_FINE_LOCATION below that, on top of the manifest entries, plus
   * (API 33+) POST_NOTIFICATIONS for BleWatchService's background-reconnect
   * notification to actually show — the service itself still runs and
   * reconnects without it, the notification just silently won't appear. iOS
   * has no equivalent call for any of this — the Info.plist usage strings
   * alone trigger their prompts on first use. */
  const ensureBlePermissions = useCallback(async (): Promise<boolean> => {
    if (Platform.OS !== 'android') return true;
    const permissions =
      Platform.Version >= 31
        ? [
            PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
            PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
          ]
        : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];
    if (Platform.Version >= 33) {
      permissions.push(PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS);
    }
    const results = await PermissionsAndroid.requestMultiple(permissions);
    return permissions.every(
      p => results[p] === PermissionsAndroid.RESULTS.GRANTED,
    );
  }, []);

  const stopScan = useCallback(() => {
    stopScanRef.current?.();
    stopScanRef.current = null;
    if (scanTimerRef.current) {
      clearTimeout(scanTimerRef.current);
      scanTimerRef.current = null;
    }
  }, []);

  const connectAndPair = useCallback(
    async (device: DeviceDescriptor, id: Identity): Promise<void> => {
      if (connectingRef.current) return;
      connectingRef.current = true;
      stopScan();
      setErrorMsg(null);
      setStatus('connecting');
      try {
        await connectManually(device, id);
        /* No further action here: BoardSession is now 'connected', and
         * App.tsx (subscribed to BoardSession directly) is what notices
         * that and swaps this screen out. */
      } catch (err) {
        setStatus('error');
        setErrorMsg(err instanceof Error ? err.message : String(err));
      } finally {
        connectingRef.current = false;
      }
    },
    [stopScan],
  );

  const startScan = useCallback(async (): Promise<void> => {
    if (connectingRef.current) return;
    const granted = await ensureBlePermissions();
    if (!granted) {
      setStatus('error');
      setErrorMsg(
        'Bluetooth permission denied. Enable it for MOTO-CTRL in system settings, then scan again.',
      );
      return;
    }
    /* Tapping Scan while one is already running (or still waiting on the
     * radio) must not leave the old subscription behind. */
    stopScan();
    setErrorMsg(null);
    setWaitingMsg(null);
    setFound([]);
    setStatus('waiting');

    const ble = new BlePlxTransport();
    stopScanRef.current = ble.scan(
      device => {
        setFound(prev =>
          prev.some(d => d.id === device.id) ? prev : [...prev, device],
        );
      },
      scanStatus => {
        if (scanStatus.state === 'scanning') {
          setWaitingMsg(null);
          setStatus(prev => (prev === 'waiting' ? 'scanning' : prev));
          /* Start the clock here: the radio may have taken a second or two to
           * come up, and that shouldn't eat the search window. */
          if (scanTimerRef.current) clearTimeout(scanTimerRef.current);
          scanTimerRef.current = setTimeout(() => {
            stopScan();
            setStatus(prev => (prev === 'scanning' ? 'idle' : prev));
          }, SCAN_MS);
          return;
        }
        if (scanStatus.state === 'waiting') {
          setWaitingMsg(scanStatus.message);
          setStatus(prev =>
            prev === 'scanning' || prev === 'waiting' ? 'waiting' : prev,
          );
          return;
        }
        stopScan();
        setStatus('error');
        setErrorMsg(scanStatus.message);
      },
    );
  }, [ensureBlePermissions, stopScan]);

  /* Load identity, remember which board we last used, start looking, and
   * (re)arm BoardSession's watcher — a no-op if it's already running (e.g.
   * app boot already started it in index.js), but the thing that resumes it
   * after a rider lands back here from an explicit Disconnect. The known
   * device itself is never auto-connected from this screen's own scan
   * results (see this file's header comment) — only BoardSession does
   * that, and only for the remembered board specifically: auto-connecting
   * to an unrecognised one would run trust-on-first-use enrollment
   * (docs/PROTOCOL.md §6) and silently make this phone a key for someone
   * else's bike parked nearby, so an unknown board always waits for a
   * deliberate tap regardless of which path is connecting it. */
  useEffect(() => {
    let cancelled = false;
    startBoardSession();
    void (async () => {
      const [id, last] = await Promise.all([
        loadOrCreateIdentity(),
        loadLastDevice(),
      ]);
      if (cancelled) return;
      setIdentity(id);
      setKnownDeviceId(last?.id ?? null);
      void startScan();
    })();
    return () => {
      cancelled = true;
      stopScan();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const looking = status === 'scanning' || status === 'waiting';

  const statusLine =
    status === 'connecting'
      ? 'Connecting…'
      : status === 'authenticating'
        ? 'Authenticating…'
        : status === 'waiting'
          ? (waitingMsg ?? 'Waiting for Bluetooth…')
          : status === 'scanning'
            ? 'Looking for your board…'
            : null;

  return (
    <Screen
      title="MOTO-CTRL"
      trailing={
        <Button
          label={looking ? 'Scanning' : 'Scan'}
          tone="secondary"
          busy={looking}
          disabled={busy}
          onPress={() => void startScan()}
        />
      }
    >
      {notice && <Notice tone="warn">{notice}</Notice>}
      {status === 'error' && errorMsg && (
        <Notice tone="danger">{errorMsg}</Notice>
      )}
      {statusLine && (
        <Notice tone={status === 'waiting' ? 'warn' : 'info'}>
          {statusLine}
        </Notice>
      )}

      <SectionHeader
        hint={
          knownDeviceId
            ? 'Your paired board connects on its own as soon as it is in range.'
            : 'Tap your board to pair this phone with it.'
        }
      >
        Boards
      </SectionHeader>

      {found.length === 0 ? (
        <Card>
          <EmptyState
            title={
              status === 'waiting'
                ? 'Waiting for Bluetooth'
                : looking
                  ? 'Searching…'
                  : 'No boards found'
            }
            body={
              status === 'waiting'
                ? (waitingMsg ?? undefined)
                : looking
                  ? 'Make sure the board has power and is within a few metres.'
                  : 'Check the board has power, then scan again.'
            }
          />
        </Card>
      ) : (
        found.map(d => {
          const known = d.id === knownDeviceId;
          return (
            <Card key={d.id} padded={false}>
              <Pressable
                onPress={() => identity && void connectAndPair(d, identity)}
                disabled={busy || !identity}
                accessibilityRole="button"
                accessibilityLabel={`${d.name}${known ? ', your paired board' : ''}`}
                style={({ pressed }) => [
                  styles.deviceRow,
                  pressed && styles.deviceRowPressed,
                  (busy || !identity) && styles.dim,
                ]}
              >
                {/* Name only. The transport id underneath was a CoreBluetooth
                 * UUID (or a MAC on Android) — meaningless to a rider, and
                 * never the thing they identify their bike by. Boards are
                 * renameable now, which is the real answer to telling two
                 * apart. */}
                <View style={styles.deviceText}>
                  <Text style={type.heading} numberOfLines={1}>
                    {d.name}
                  </Text>
                </View>
                {known && <Badge label="PAIRED" tone="on" />}
              </Pressable>
            </Card>
          );
        })
      )}
    </Screen>
  );
}

const styles = StyleSheet.create({
  deviceRow: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    padding: space.md,
    minHeight: 60,
    borderRadius: radius.lg,
  },
  deviceRowPressed: { backgroundColor: colors.raisedHover },
  dim: { opacity: 0.5 },
  deviceText: { flex: 1 },
});
