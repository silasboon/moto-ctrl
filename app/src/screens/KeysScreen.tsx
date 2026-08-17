/**
 * Paired keys: list enrolled phones (KEY_LIST), revoke one (KEY_REVOKE),
 * and enroll another phone by pasting the base64 public key its own
 * Pairing screen displays. This is a deliberate MVP simplification — no
 * QR/camera flow, no extra native dependency for it — not a gap in
 * phone-as-key auth ("support multiple paired phones"),
 * which this still satisfies end to end.
 */
import React, { useCallback, useEffect, useState } from 'react';
import { StyleSheet, Text, TouchableOpacity, View } from 'react-native';

import {
  defaultKeyLabel,
  loadOrCreateIdentity,
  saveIdentity,
  type Identity,
} from '../identity/KeyStore';
import { base64ToBytes } from '../protocol/frames';
import type { MotoClient } from '../protocol/MotoClient';
import type { EnrolledKey } from '../protocol/types';
import {
  Button,
  Card,
  Field,
  Input,
  Notice,
  Screen,
  SectionHeader,
  SkeletonCard,
} from '../ui/components';
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

  /* This phone's own key. Edited here rather than on the pairing screen,
   * which is gone the moment a board connects — the one place a rider would
   * never be able to come back to. */
  const [identity, setIdentity] = useState<Identity | null>(null);
  const [renaming, setRenaming] = useState(false);
  useEffect(() => {
    void loadOrCreateIdentity().then(setIdentity);
  }, []);

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

  /* Renaming means re-sending ENROLL with this phone's own public key: the
   * board's keystore treats an add of a key it already holds as a label
   * update on the existing slot, so there is no rename opcode to add and no
   * new slot consumed. It needs an authenticated session, which is exactly
   * what this screen already has. */
  async function renameThisPhone(): Promise<void> {
    if (!identity) return;
    const label = identity.label.trim() || defaultKeyLabel();
    setRenaming(true);
    setError(null);
    setMessage(null);
    try {
      const next = { ...identity, label };
      setIdentity(next);
      await saveIdentity(next);
      const result = await client.enroll(next.keypair.publicKey, label);
      setMessage(
        result.ok
          ? 'Name updated on the board.'
          : `Saved on this phone, but the board rejected it: ${result.resultName}`,
      );
      if (result.ok) await refresh();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setRenaming(false);
    }
  }

  return (
    <Screen title="Paired Keys" onBack={onDone}>
      <SectionHeader hint="How this phone appears in the list below.">
        This phone
      </SectionHeader>
      <Card>
        <Field
          label="Key name"
          value={identity?.label ?? ''}
          editable={!!identity}
          onChangeText={v => identity && setIdentity({ ...identity, label: v })}
          placeholder={defaultKeyLabel()}
          maxLength={31}
          autoCapitalize="words"
          autoCorrect={false}
        />
        <Button
          label={renaming ? 'Saving' : 'Save name'}
          tone="secondary"
          busy={renaming}
          disabled={!identity}
          onPress={renameThisPhone}
        />
      </Card>
      {/* iOS won't hand over "Silas's iPhone" — see defaultKeyLabel — so on a
       * shared bike both phones enrol as "iPhone" unless someone types
       * something. Worth saying once, here, where it can be fixed. */}
      <Notice tone="info">
        Your phone can only tell the app its model, not the name you gave it, so
        it is worth setting this if more than one phone unlocks this bike.
      </Notice>

      <SectionHeader>Enrolled</SectionHeader>

      {loading ? (
        <SkeletonCard lines={1} />
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
      <Input
        style={styles.input}
        value={newPubkeyB64}
        onChangeText={setNewPubkeyB64}
        placeholder="base64 public key"
        autoCapitalize="none"
        autoCorrect={false}
      />
      <Input
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
    </Screen>
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
