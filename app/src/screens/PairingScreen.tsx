/**
 * Pairing screen: connect to a board (simulator or real BLE) and either
 * authenticate (already enrolled) or enroll (first pairing / TOFU) this
 * phone's identity. See docs/PROTOCOL.md §6 for the enroll/auth rules this
 * mirrors, and firmware/sim/gui/app.js for the reference flow.
 */
import React, { useEffect, useState } from 'react';
import { ActivityIndicator, ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';

import { loadLastDevice, loadOrCreateIdentity, saveLastDevice, type Identity } from '../identity/KeyStore';
import { MotoClient } from '../protocol/MotoClient';
import { bytesToBase64 } from '../protocol/frames';
import { BlePlxTransport } from '../transport/BlePlxTransport';
import { SimTransport } from '../transport/SimTransport';
import type { DeviceDescriptor } from '../transport/Transport';

type Mode = 'sim' | 'ble';
type PairingStatus = 'idle' | 'connecting' | 'authenticating' | 'enrolling' | 'error';

interface Props {
  onPaired: (client: MotoClient, device: DeviceDescriptor) => void;
}

export function PairingScreen({ onPaired }: Props): React.JSX.Element {
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [mode, setMode] = useState<Mode>('sim');
  const [simUrl, setSimUrl] = useState('ws://127.0.0.1:8010');
  const [foundDevices, setFoundDevices] = useState<DeviceDescriptor[]>([]);
  const [scanning, setScanning] = useState(false);
  const [status, setStatus] = useState<PairingStatus>('idle');
  const [errorMsg, setErrorMsg] = useState<string | null>(null);

  useEffect(() => {
    loadOrCreateIdentity().then(setIdentity);
    loadLastDevice().then((last) => {
      if (last) {
        setSimUrl(last.id.startsWith('ws://') || last.id.startsWith('wss://') ? last.id : simUrl);
      }
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  function startScan(): void {
    setScanning(true);
    setFoundDevices([]);
    const ble = new BlePlxTransport();
    const stop = ble.scan((device) => {
      setFoundDevices((prev) => (prev.some((d) => d.id === device.id) ? prev : [...prev, device]));
    });
    setTimeout(() => {
      stop();
      setScanning(false);
    }, 8000);
  }

  async function connectAndPair(deviceId: string, device: DeviceDescriptor): Promise<void> {
    if (!identity) {
      return;
    }
    setErrorMsg(null);
    setStatus('connecting');
    const transport = mode === 'sim' ? new SimTransport(deviceId) : new BlePlxTransport();
    const client = new MotoClient(transport);
    try {
      await client.connect(deviceId);

      setStatus('authenticating');
      let authResult = await client.authenticate(identity.keypair);

      if (!authResult.ok) {
        setStatus('enrolling');
        const enrollResult = await client.enroll(identity.keypair.publicKey, identity.label);
        if (!enrollResult.ok) {
          throw new Error(
            `Enrollment denied (${enrollResult.resultName}). This board already has paired phones — ` +
              'ask one of them to enroll this phone from its Keys screen using the public key below.',
          );
        }
        setStatus('authenticating');
        authResult = await client.authenticate(identity.keypair);
        if (!authResult.ok) {
          throw new Error(`Authentication failed after enrollment (${authResult.resultName}).`);
        }
      }

      await saveLastDevice({ id: deviceId, name: device.name });
      onPaired(client, device);
    } catch (err) {
      setStatus('error');
      setErrorMsg(err instanceof Error ? err.message : String(err));
      await client.disconnect().catch(() => {});
    }
  }

  const busy = status === 'connecting' || status === 'authenticating' || status === 'enrolling';

  return (
    <ScrollView contentContainerStyle={styles.container}>
      <Text style={styles.title}>MOTO-CTRL</Text>
      <Text style={styles.subtitle}>Pair with a board</Text>

      <View style={styles.modeRow}>
        <TouchableOpacity
          style={[styles.modeButton, mode === 'sim' && styles.modeButtonActive]}
          onPress={() => setMode('sim')}
          disabled={busy}
        >
          <Text style={mode === 'sim' ? styles.modeTextActive : styles.modeText}>Simulator</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[styles.modeButton, mode === 'ble' && styles.modeButtonActive]}
          onPress={() => setMode('ble')}
          disabled={busy}
        >
          <Text style={mode === 'ble' ? styles.modeTextActive : styles.modeText}>Bluetooth</Text>
        </TouchableOpacity>
      </View>

      {mode === 'sim' ? (
        <View style={styles.section}>
          <Text style={styles.label}>Simulator URL</Text>
          <TextInput
            style={styles.input}
            value={simUrl}
            onChangeText={setSimUrl}
            autoCapitalize="none"
            autoCorrect={false}
            editable={!busy}
            placeholder="ws://127.0.0.1:8010"
          />
          <TouchableOpacity
            style={[styles.connectButton, busy && styles.disabledButton]}
            onPress={() => connectAndPair(simUrl, { id: simUrl, name: `sim: ${simUrl}` })}
            disabled={busy || !identity}
          >
            <Text style={styles.connectButtonText}>Connect</Text>
          </TouchableOpacity>
        </View>
      ) : (
        <View style={styles.section}>
          <TouchableOpacity style={styles.connectButton} onPress={startScan} disabled={scanning || busy}>
            <Text style={styles.connectButtonText}>{scanning ? 'Scanning…' : 'Scan for MOTO-CTRL'}</Text>
          </TouchableOpacity>
          {foundDevices.map((d) => (
            <TouchableOpacity
              key={d.id}
              style={styles.deviceRow}
              onPress={() => connectAndPair(d.id, d)}
              disabled={busy}
            >
              <Text style={styles.deviceName}>{d.name}</Text>
              <Text style={styles.deviceId}>{d.id}</Text>
            </TouchableOpacity>
          ))}
        </View>
      )}

      {busy && (
        <View style={styles.statusRow}>
          <ActivityIndicator />
          <Text style={styles.statusText}>{status === 'connecting' ? 'Connecting…' : status === 'enrolling' ? 'Enrolling…' : 'Authenticating…'}</Text>
        </View>
      )}
      {status === 'error' && errorMsg && <Text style={styles.error}>{errorMsg}</Text>}

      {identity && (
        <View style={styles.identityBox}>
          <Text style={styles.label}>This phone's public key (for the "enroll another phone" flow)</Text>
          <Text selectable style={styles.pubkey}>
            {bytesToBase64(identity.keypair.publicKey)}
          </Text>
        </View>
      )}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flexGrow: 1, padding: 20, gap: 12 },
  title: { fontSize: 28, fontWeight: '700', textAlign: 'center' },
  subtitle: { fontSize: 14, color: '#666', textAlign: 'center', marginBottom: 12 },
  modeRow: { flexDirection: 'row', gap: 8, marginBottom: 12 },
  modeButton: { flex: 1, padding: 10, borderRadius: 8, borderWidth: 1, borderColor: '#ccc', alignItems: 'center' },
  modeButtonActive: { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  modeText: { color: '#333' },
  modeTextActive: { color: 'white', fontWeight: '600' },
  section: { gap: 8 },
  label: { fontSize: 12, color: '#666', textTransform: 'uppercase' },
  input: { borderWidth: 1, borderColor: '#ccc', borderRadius: 8, padding: 10, fontFamily: 'Menlo' },
  connectButton: { backgroundColor: '#2563eb', padding: 12, borderRadius: 8, alignItems: 'center' },
  disabledButton: { opacity: 0.5 },
  connectButtonText: { color: 'white', fontWeight: '600' },
  deviceRow: { padding: 10, borderWidth: 1, borderColor: '#ddd', borderRadius: 8, marginTop: 8 },
  deviceName: { fontWeight: '600' },
  deviceId: { fontSize: 11, color: '#888' },
  statusRow: { flexDirection: 'row', alignItems: 'center', gap: 8, marginTop: 8 },
  statusText: { color: '#666' },
  error: { color: '#b91c1c', marginTop: 8 },
  identityBox: { marginTop: 24, padding: 12, borderRadius: 8, backgroundColor: '#f4f5f7' },
  pubkey: { fontFamily: 'Menlo', fontSize: 11, marginTop: 4 },
});
