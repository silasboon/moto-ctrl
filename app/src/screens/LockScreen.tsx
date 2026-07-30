/**
 * Lock / immobilizer settings (docs/PROTOCOL.md §11): enable/
 * disable the immobilizer, pick unlock methods, set the button cheat-code,
 * and transfer ownership.
 *
 * Follows PinMapperScreen's read-edit-save pattern for the config half, and
 * KeysScreen's pattern for the destructive-action confirmation half
 * (ownership transfer). No <Picker> — same chip-button convention as
 * PinMapperScreen.
 *
 * The cheat-code itself is write-only from the app's perspective: the
 * device only ever returns whether one is set and how long it is (never
 * the sequence, never a hash) — see LockConfig. This screen's "Set
 * cheat-code" field is local-only input, cleared after a successful save;
 * there is nothing to "read back" and display.
 */
import React, { useEffect, useState } from 'react';
import { StyleSheet, Switch, Text, TouchableOpacity, View } from 'react-native';

import { LOCK_METHOD } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import { defaultLockConfig, type LockConfig } from '../protocol/types';
import {
  Input,
  KeyboardAwareScroll,
  NumberInput,
  useLeaveGuard,
} from '../ui/components';
import { colors } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
  onOwnershipTransferred: () => void;
}

function Chip({
  label,
  active,
  onPress,
}: {
  label: string;
  active: boolean;
  onPress: () => void;
}): React.JSX.Element {
  return (
    <TouchableOpacity
      style={[styles.chip, active && styles.chipActive]}
      onPress={onPress}
    >
      <Text style={active ? styles.chipTextActive : styles.chipText}>
        {label}
      </Text>
    </TouchableOpacity>
  );
}

/** Parses "0,1,2,3" style input into button indices, silently dropping
 * anything out of the 0-7 range so a stray character doesn't produce a
 * confusing device-side BAD_REQUEST. */
function parseCode(text: string): number[] {
  return text
    .split(',')
    .map(s => parseInt(s.trim(), 10))
    .filter(n => Number.isInteger(n) && n >= 0 && n <= 7);
}

export function LockScreen({
  client,
  onDone,
  onOwnershipTransferred,
}: Props): React.JSX.Element {
  const [config, setConfig] = useState<LockConfig>(defaultLockConfig());
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saveResult, setSaveResult] = useState<string | null>(null);

  const [codeText, setCodeText] = useState('');
  const [codeBusy, setCodeBusy] = useState(false);
  const [codeResult, setCodeResult] = useState<string | null>(null);

  const [testText, setTestText] = useState('');
  const [testResult, setTestResult] = useState<string | null>(null);

  const [transferring, setTransferring] = useState(false);
  const [confirmTransfer, setConfirmTransfer] = useState(false);

  /* Serialised copy of what the board last told us — see OutputsScreen. The
   * cheat-code field is deliberately NOT part of this: it is write-only and
   * has its own Set action, so an unsent code is not a config edit. */
  const [baseline, setBaseline] = useState<string | null>(null);

  useEffect(() => {
    client
      .lockGetConfig()
      .then(c => {
        setConfig(c);
        setBaseline(JSON.stringify(c));
      })
      .catch((err: unknown) =>
        setError(err instanceof Error ? err.message : String(err)),
      )
      .finally(() => setLoading(false));
  }, [client]);

  async function refreshConfig(): Promise<void> {
    try {
      const fresh = await client.lockGetConfig();
      setConfig(fresh);
      setBaseline(JSON.stringify(fresh));
    } catch {
      // Keep showing the last-known config; the error surfaces from
      // whichever action triggered this refresh instead.
    }
  }

  async function saveConfig(): Promise<void> {
    setSaving(true);
    setError(null);
    setSaveResult(null);
    try {
      const result = await client.lockSetConfig(config);
      if (result.ok) {
        setSaveResult('Saved.');
      } else {
        setSaveResult(`Rejected by device: ${result.resultName}`);
      }
      await refreshConfig();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  async function setCheatcode(): Promise<void> {
    const buttons = parseCode(codeText);
    if (buttons.length < 4 || buttons.length > 10) {
      setCodeResult(`Need 4-10 button indices (0-7), got ${buttons.length}.`);
      return;
    }
    setCodeBusy(true);
    setCodeResult(null);
    try {
      const result = await client.cheatcodeSet(buttons);
      setCodeResult(
        result.ok ? 'Cheat-code set.' : `Rejected: ${result.resultName}`,
      );
      if (result.ok) {
        setCodeText('');
        await refreshConfig();
      }
    } catch (err) {
      setCodeResult(err instanceof Error ? err.message : String(err));
    } finally {
      setCodeBusy(false);
    }
  }

  async function clearCheatcode(): Promise<void> {
    setCodeBusy(true);
    setCodeResult(null);
    try {
      const result = await client.cheatcodeClear();
      setCodeResult(
        result.ok
          ? 'Cheat-code cleared.'
          : `Rejected: ${result.resultName} — disable the immobilizer first (it's the mandatory fallback while enabled).`,
      );
      if (result.ok) {
        await refreshConfig();
      }
    } catch (err) {
      setCodeResult(err instanceof Error ? err.message : String(err));
    } finally {
      setCodeBusy(false);
    }
  }

  async function testCheatcode(): Promise<void> {
    const buttons = parseCode(testText);
    if (buttons.length === 0) {
      setTestResult('Enter a candidate sequence to test.');
      return;
    }
    try {
      const result = await client.cheatcodeTest(buttons);
      setTestResult(
        !result.ok
          ? 'Device rejected the request.'
          : result.match
            ? 'MATCH'
            : 'No match.',
      );
    } catch (err) {
      setTestResult(err instanceof Error ? err.message : String(err));
    }
  }

  async function doTransferOwnership(): Promise<void> {
    setTransferring(true);
    try {
      const result = await client.transferOwnership();
      if (result.ok) {
        onOwnershipTransferred();
      } else {
        setError(`Ownership transfer rejected: ${result.resultName}`);
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setTransferring(false);
      setConfirmTransfer(false);
    }
  }

  const dirty = baseline !== null && JSON.stringify(config) !== baseline;
  const back = useLeaveGuard(dirty, onDone);

  if (loading) {
    return (
      <View style={styles.center}>
        <Text>Loading…</Text>
      </View>
    );
  }

  const methodPhone = (config.methodsMask & LOCK_METHOD.PHONE) !== 0;
  const methodIgnSwitch =
    (config.methodsMask & LOCK_METHOD.IGNITION_SWITCH) !== 0;

  function toggleMethod(bit: number, on: boolean): void {
    setConfig(prev => ({
      ...prev,
      methodsMask: on ? prev.methodsMask | bit : prev.methodsMask & ~bit,
    }));
  }

  return (
    <KeyboardAwareScroll
      style={styles.container}
      contentContainerStyle={styles.content}
    >
      <View style={styles.header}>
        <Text style={styles.title}>Lock / Immobilizer</Text>
        <TouchableOpacity onPress={back}>
          <Text style={styles.link}>Back</Text>
        </TouchableOpacity>
      </View>

      <View style={styles.row}>
        <Text style={styles.rowLabel}>Immobilizer enabled</Text>
        <Switch
          value={config.immobilizerEnabled}
          onValueChange={v =>
            setConfig(prev => ({ ...prev, immobilizerEnabled: v }))
          }
        />
      </View>
      {config.immobilizerEnabled && !config.cheatcodeSet && (
        <Text style={styles.warn}>
          A cheat-code must be set (below) before this can be enabled —
          it&apos;s the mandatory fallback and can never be turned off while the
          immobilizer is on.
        </Text>
      )}

      <Text style={styles.sectionTitle}>Unlock methods</Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Phone-as-key</Text>
        <Switch
          value={methodPhone}
          onValueChange={v => toggleMethod(LOCK_METHOD.PHONE, v)}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Ignition switch</Text>
        <Switch
          value={methodIgnSwitch}
          onValueChange={v => toggleMethod(LOCK_METHOD.IGNITION_SWITCH, v)}
        />
      </View>
      <Text style={styles.hint}>
        The button cheat-code is always active whenever the immobilizer is
        enabled — it&apos;s not a toggle.
      </Text>
      {methodIgnSwitch && (
        <>
          <Text style={styles.sectionTitle}>Ignition-switch input</Text>
          <View style={styles.chipRow}>
            <Chip
              label="none"
              active={config.ignitionSwitchInput === -1}
              onPress={() =>
                setConfig(prev => ({ ...prev, ignitionSwitchInput: -1 }))
              }
            />
            {Array.from({ length: 8 }).map((_, i) => (
              <Chip
                key={i}
                label={`input ${i}`}
                active={config.ignitionSwitchInput === i}
                onPress={() =>
                  setConfig(prev => ({ ...prev, ignitionSwitchInput: i }))
                }
              />
            ))}
          </View>
        </>
      )}

      <Text style={styles.sectionTitle}>Timing</Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Auto-lock grace (ms)</Text>
        <NumberInput
          style={styles.numInput}
          value={config.autoLockGraceMs}
          min={0}
          onChangeValue={v =>
            setConfig(prev => ({ ...prev, autoLockGraceMs: v }))
          }
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Cheat-code entry window (ms)</Text>
        <NumberInput
          style={styles.numInput}
          value={config.cheatcodeWindowMs}
          min={0}
          onChangeValue={v =>
            setConfig(prev => ({ ...prev, cheatcodeWindowMs: v }))
          }
        />
      </View>

      {error && <Text style={styles.error}>{error}</Text>}
      {saveResult && <Text style={styles.success}>{saveResult}</Text>}
      <TouchableOpacity
        style={[styles.saveButton, saving && styles.disabled]}
        onPress={saveConfig}
        disabled={saving}
      >
        <Text style={styles.saveButtonText}>{saving ? 'Saving…' : 'Save'}</Text>
      </TouchableOpacity>

      <Text style={styles.sectionTitle}>Cheat-code</Text>
      <Text style={styles.hint}>
        {config.cheatcodeSet
          ? `Set (${config.cheatcodeLen} presses). Entered physically on the handlebar buttons while the bike is
             locked — this app never reads it back.`
          : 'Not set.'}
      </Text>
      <Input
        style={styles.nameInput}
        value={codeText}
        onChangeText={setCodeText}
        placeholder="new code, e.g. 0,1,2,3 (4-10 button indices, 0-7)"
      />
      <View style={styles.actionRow}>
        <TouchableOpacity
          style={[styles.smallButton, codeBusy && styles.disabled]}
          onPress={setCheatcode}
          disabled={codeBusy}
        >
          <Text style={styles.smallButtonText}>Set</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[
            styles.smallButton,
            styles.dangerButton,
            codeBusy && styles.disabled,
          ]}
          onPress={clearCheatcode}
          disabled={codeBusy}
        >
          <Text style={styles.smallButtonText}>Clear</Text>
        </TouchableOpacity>
      </View>
      {codeResult && <Text style={styles.hint}>{codeResult}</Text>}

      <Text style={styles.sectionTitle}>
        Test a candidate (practice — no side effects)
      </Text>
      <Input
        style={styles.nameInput}
        value={testText}
        onChangeText={setTestText}
        placeholder="e.g. 0,1,2,3"
      />
      <TouchableOpacity style={styles.smallButton} onPress={testCheatcode}>
        <Text style={styles.smallButtonText}>Test</Text>
      </TouchableOpacity>
      {testResult && <Text style={styles.hint}>{testResult}</Text>}

      <Text style={styles.sectionTitle}>Ownership transfer</Text>
      <Text style={styles.hint}>
        Wipes every enrolled phone key and the cheat-code, back to a
        factory-fresh, re-enrollable state. Use this before selling or handing
        off the bike.
      </Text>
      {!confirmTransfer ? (
        <TouchableOpacity
          style={[styles.smallButton, styles.dangerButton]}
          onPress={() => setConfirmTransfer(true)}
        >
          <Text style={styles.smallButtonText}>Transfer ownership…</Text>
        </TouchableOpacity>
      ) : (
        <View style={styles.confirmBlock}>
          <Text style={styles.warn}>
            This immediately revokes every paired phone (including this one) and
            disables the immobilizer. This cannot be undone from the app.
          </Text>
          <View style={styles.actionRow}>
            <TouchableOpacity
              style={[
                styles.smallButton,
                styles.dangerButton,
                transferring && styles.disabled,
              ]}
              onPress={doTransferOwnership}
              disabled={transferring}
            >
              <Text style={styles.smallButtonText}>
                {transferring ? 'Transferring…' : 'Confirm transfer'}
              </Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={styles.smallButton}
              onPress={() => setConfirmTransfer(false)}
              disabled={transferring}
            >
              <Text style={styles.smallButtonText}>Cancel</Text>
            </TouchableOpacity>
          </View>
        </View>
      )}
    </KeyboardAwareScroll>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: colors.bg },
  content: { padding: 16, gap: 10 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: { color: colors.text, fontSize: 20, fontWeight: '700' },
  link: { color: colors.accent },
  row: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    paddingVertical: 4,
  },
  rowLabel: { color: colors.text, fontSize: 14 },
  sectionTitle: {
    fontSize: 13,
    color: colors.textMuted,
    textTransform: 'uppercase',
    marginTop: 10,
  },
  hint: { fontSize: 12, color: colors.textFaint },
  warn: { fontSize: 12, color: colors.warn },
  chipRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  chip: {
    paddingVertical: 6,
    paddingHorizontal: 10,
    borderRadius: 999,
    borderWidth: 1,
    borderColor: colors.borderStrong,
  },
  chipActive: { backgroundColor: colors.accent, borderColor: colors.accent },
  chipText: { fontSize: 12, color: colors.text },
  chipTextActive: {
    fontSize: 12,
    color: colors.textOnAccent,
    fontWeight: '600',
  },
  nameInput: {
    borderWidth: 1,
    borderColor: colors.borderStrong,
    borderRadius: 6,
    padding: 8,
    color: colors.text,
  },
  numInput: {
    borderWidth: 1,
    borderColor: colors.borderStrong,
    borderRadius: 6,
    padding: 8,
    width: 90,
    textAlign: 'right',
    color: colors.text,
  },
  error: { color: colors.danger },
  success: { color: colors.on },
  saveButton: {
    backgroundColor: colors.accent,
    padding: 12,
    borderRadius: 8,
    alignItems: 'center',
    marginTop: 4,
  },
  disabled: { opacity: 0.5 },
  saveButtonText: { color: colors.textOnAccent, fontWeight: '600' },
  actionRow: { flexDirection: 'row', gap: 8 },
  smallButton: {
    paddingVertical: 8,
    paddingHorizontal: 12,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.accent,
    alignItems: 'center',
  },
  dangerButton: { borderColor: colors.danger },
  smallButtonText: { color: colors.accent, fontWeight: '600' },
  dangerButtonText: { color: colors.danger, fontWeight: '600' },
  confirmBlock: {
    borderWidth: 1,
    borderColor: colors.danger,
    borderRadius: 8,
    padding: 10,
    gap: 8,
  },
});
