/**
 * Pairing: find a board over BLE and either authenticate (already enrolled)
 * or enroll this phone (first pairing / TOFU). See docs/PROTOCOL.md §6.
 *
 * Scanning starts on its own as soon as the screen appears, because with a
 * board on the bike there is exactly one thing a rider ever wants here.
 *
 * The simulator transport is deliberately NOT offered. SimTransport still
 * exists and is still required — AGENTS.md mandates the two-implementation
 * transport split, and the app's CI integration test drives the real
 * firmware/sim through it — it simply has no place in the rider-facing UI now
 * that hardware exists.
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
  loadLastDevice,
  loadOrCreateIdentity,
  saveLastDevice,
  type Identity,
} from '../identity/KeyStore';
import { MotoClient } from '../protocol/MotoClient';
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
  | 'enrolling'
  | 'error';

interface Props {
  onPaired: (client: MotoClient, device: DeviceDescriptor) => void;
  /** Shown once at the top — e.g. "the board disconnected" after a drop. */
  notice?: string | null;
}

export function PairingScreen({ onPaired, notice }: Props): React.JSX.Element {
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [knownDeviceId, setKnownDeviceId] = useState<string | null>(null);
  const [found, setFound] = useState<DeviceDescriptor[]>([]);
  const [status, setStatus] = useState<PairingStatus>('idle');
  const [errorMsg, setErrorMsg] = useState<string | null>(null);
  /** Why the radio isn't scanning yet — "Bluetooth is off", and so on. */
  const [waitingMsg, setWaitingMsg] = useState<string | null>(null);

  const stopScanRef = useRef<(() => void) | null>(null);
  const scanTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** Guards auto-connect so a re-render or a second advertisement can't start
   * a second connect for the same board. */
  const connectingRef = useRef(false);

  const busy =
    status === 'connecting' ||
    status === 'authenticating' ||
    status === 'enrolling';

  /** Android needs runtime grants for BLUETOOTH_SCAN/CONNECT (API 31+) or
   * ACCESS_FINE_LOCATION below that, on top of the manifest entries. iOS has
   * no equivalent call — the Info.plist usage string alone triggers its
   * prompt on first CoreBluetooth use. */
  const ensureBlePermissions = useCallback(async (): Promise<boolean> => {
    if (Platform.OS !== 'android') return true;
    const permissions =
      Platform.Version >= 31
        ? [
            PermissionsAndroid.PERMISSIONS.BLUETOOTH_SCAN,
            PermissionsAndroid.PERMISSIONS.BLUETOOTH_CONNECT,
          ]
        : [PermissionsAndroid.PERMISSIONS.ACCESS_FINE_LOCATION];
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

      const client = new MotoClient(new BlePlxTransport());
      try {
        await client.connect(device.id);

        setStatus('authenticating');
        let auth = await client.authenticate(id.keypair);

        if (!auth.ok) {
          setStatus('enrolling');
          const enrolled = await client.enroll(id.keypair.publicKey, id.label);
          if (!enrolled.ok) {
            throw new Error(
              `This board already has paired phones (${enrolled.resultName}). ` +
                'Ask a paired phone to add this one from its Paired Keys screen.',
            );
          }
          setStatus('authenticating');
          auth = await client.authenticate(id.keypair);
          if (!auth.ok) {
            throw new Error(
              `Authentication failed after pairing (${auth.resultName}).`,
            );
          }
        }

        await saveLastDevice({ id: device.id, name: device.name });
        onPaired(client, device);
      } catch (err) {
        setStatus('error');
        setErrorMsg(err instanceof Error ? err.message : String(err));
        await client.disconnect().catch(() => {});
      } finally {
        connectingRef.current = false;
      }
    },
    [onPaired, stopScan],
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

  /* Load identity, remember which board we last used, and start looking. */
  useEffect(() => {
    let cancelled = false;
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

  /* Reconnect to the board this phone already owns as soon as it appears.
   *
   * Deliberately ONLY the remembered board. Auto-connecting to an
   * unrecognised one would run trust-on-first-use enrollment
   * (docs/PROTOCOL.md §6) and silently make this phone a key for someone
   * else's bike parked nearby — an unknown board always waits for a
   * deliberate tap. */
  useEffect(() => {
    if (!identity || !knownDeviceId || connectingRef.current) return;
    const match = found.find(d => d.id === knownDeviceId);
    if (match) void connectAndPair(match, identity);
  }, [found, identity, knownDeviceId, connectAndPair]);

  const looking = status === 'scanning' || status === 'waiting';

  const statusLine =
    status === 'connecting'
      ? 'Connecting…'
      : status === 'authenticating'
        ? 'Authenticating…'
        : status === 'enrolling'
          ? 'Pairing this phone…'
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
