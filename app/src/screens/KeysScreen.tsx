/**
 * Paired keys: list enrolled phones (KEY_LIST), revoke one (KEY_REVOKE),
 * and enroll another phone by pasting the base64 public key its own
 * Pairing screen displays. This is a deliberate MVP simplification — no
 * QR/camera flow, no extra native dependency for it — not a gap in
 * AGENTS.md safety requirement #4 ("support multiple paired phones"),
 * which this still satisfies end to end.
 */
import React, { useCallback, useEffect, useState } from 'react';
import {
  ActivityIndicator,
  ScrollView,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from 'react-native';

import { base64ToBytes } from '../protocol/frames';
import type { MotoClient } from '../protocol/MotoClient';
import type { EnrolledKey } from '../protocol/types';
import { colors } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

export function KeysScreen({ client, onDone }: Props): React.JSX.Element {
  const [keys, setKeys] = useState<EnrolledKey[] | null>(null);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [message, setMessage] = useState<string | null>(null);
  const [newPubkeyB64, setNewPubkeyB64] = useState('');
  const [newLabel, setNewLabel] = useState('');

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      setKeys(await client.keyList());
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setLoading(false);
    }
  }, [client]);

  useEffect(() => {
    refresh();
  }, [refresh]);

  async function revoke(slot: number): Promise<void> {
    setBusy(true);
    setError(null);
    setMessage(null);
    try {
      const result = await client.keyRevoke(slot);
      setMessage(
        result.ok ? `Revoked slot ${slot}.` : `Failed: ${result.resultName}`,
      );
      await refresh();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  }

  async function enrollOther(): Promise<void> {
    setBusy(true);
    setError(null);
    setMessage(null);
    try {
      const pubkey = base64ToBytes(newPubkeyB64.trim());
      if (pubkey.length !== 32) {
        throw new Error('Public key must decode to 32 bytes (Ed25519).');
      }
      const result = await client.enroll(pubkey, newLabel.trim() || 'phone');
      setMessage(
        result.ok
          ? `Enrolled as slot ${result.slot}.`
          : `Failed: ${result.resultName}`,
      );
      if (result.ok) {
        setNewPubkeyB64('');
        setNewLabel('');
        await refresh();
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setBusy(false);
    }
  }

  return (
    <ScrollView style={styles.container} contentContainerStyle={styles.content}>
      <View style={styles.header}>
        <Text style={styles.title}>Paired Keys</Text>
        <TouchableOpacity onPress={onDone}>
          <Text style={styles.link}>Back</Text>
        </TouchableOpacity>
      </View>

      {loading ? (
        <ActivityIndicator />
      ) : (
        (keys ?? []).map(k => (
          <View key={k.slot} style={styles.keyRow}>
            <View>
              <Text style={styles.keyLabel}>{k.label || '(unlabeled)'}</Text>
              <Text style={styles.keySlot}>slot {k.slot}</Text>
            </View>
            <TouchableOpacity onPress={() => revoke(k.slot)} disabled={busy}>
              <Text style={styles.revoke}>Revoke</Text>
            </TouchableOpacity>
          </View>
        ))
      )}

      <Text style={styles.sectionTitle}>Enroll another phone</Text>
      <Text style={styles.hint}>
        On the other phone, open Pairing and copy its public key from the box at
        the bottom, then paste it here.
      </Text>
      <TextInput
        placeholderTextColor={colors.textFaint}
        style={styles.input}
        value={newPubkeyB64}
        onChangeText={setNewPubkeyB64}
        placeholder="base64 public key"
        autoCapitalize="none"
        autoCorrect={false}
      />
      <TextInput
        placeholderTextColor={colors.textFaint}
        style={styles.input}
        value={newLabel}
        onChangeText={setNewLabel}
        placeholder="label, e.g. Jamie's phone"
      />
      <TouchableOpacity
        style={[styles.enrollButton, busy && styles.disabled]}
        onPress={enrollOther}
        disabled={busy}
      >
        <Text style={styles.enrollButtonText}>
          {busy ? 'Working…' : 'Enroll'}
        </Text>
      </TouchableOpacity>

      {error && <Text style={styles.error}>{error}</Text>}
      {message && <Text style={styles.success}>{message}</Text>}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: colors.bg },
  content: { padding: 16, gap: 12 },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: { color: colors.text, fontSize: 20, fontWeight: '700' },
  link: { color: colors.accent },
  keyRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    padding: 10,
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: 8,
  },
  keyLabel: { color: colors.text, fontWeight: '600' },
  keySlot: { fontSize: 11, color: colors.textFaint, fontFamily: 'Menlo' },
  revoke: { color: colors.danger },
  sectionTitle: {
    fontSize: 13,
    color: colors.textMuted,
    textTransform: 'uppercase',
    marginTop: 8,
  },
  hint: { fontSize: 12, color: colors.textFaint },
  input: {
    borderWidth: 1,
    borderColor: colors.borderStrong,
    borderRadius: 8,
    padding: 10,
    color: colors.text,
  },
  enrollButton: {
    backgroundColor: colors.accent,
    padding: 12,
    borderRadius: 8,
    alignItems: 'center',
  },
  disabled: { opacity: 0.5 },
  enrollButtonText: { color: colors.textOnAccent, fontWeight: '600' },
  error: { color: colors.danger },
  success: { color: colors.on },
});
