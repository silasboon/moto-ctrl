/**
 * Lock / immobilizer settings (docs/PROTOCOL.md §11): enable/
 * disable the immobilizer, pick unlock methods, set the button cheat-code,
 * and transfer ownership.
 *
 * Follows OutputsScreen's read-edit-save pattern for the config half, and
 * KeysScreen's pattern for the destructive-action confirmation half
 * (ownership transfer). No <Picker> — same chip-button convention as
 * OutputsScreen.
 *
 * The cheat-code itself is write-only from the app's perspective: the
 * device only ever returns whether one is set and how long it is (never
 * the sequence, never a hash) — see LockConfig. This screen's "Set
 * cheat-code" field is local-only input, cleared after a successful save;
 * there is nothing to "read back" and display.
 */
import React, { useEffect, useState } from 'react';
import { StyleSheet, Switch, Text, TouchableOpacity, View } from 'react-native';

import { INPUT_COUNT, LOCK_METHOD } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import {
  defaultLockConfig,
  type DeviceConfig,
  type LockConfig,
} from '../protocol/types';
import {
  Button,
  NumberInput,
  Screen,
  SkeletonScreen,
  useLeaveGuard,
} from '../ui/components';
import { colors } from '../ui/theme';
import { buttonLabel } from './ButtonsScreen';

/** Matches MC_LOCK_CHEATCODE_MIN_LEN / MAX_LEN (mc_lock.h). */
const CHEATCODE_MIN = 4;
const CHEATCODE_MAX = 10;

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

  /* Cheat-code capture. The code is entered on the handlebar buttons, twice,
   * rather than typed as indices.
   *
   * Typing "0,1,2,3" asked the rider to know which physical switch is input 3
   * — a mapping they have no reason to hold in their head, and which they'd
   * have to get right blind, since the code is only ever ENTERED on the
   * handlebars. Capturing it the same way it will be used means the thing
   * they practise is the thing that unlocks the bike.
   *
   * Twice, because a mistyped code is discoverable but a mis-pressed one is
   * not: the device stores a salted hash and can never read it back, so a
   * wrong code that both parties agree on is only found out at the roadside. */
  const [capture, setCapture] = useState<'idle' | 'first' | 'second' | 'test'>(
    'idle',
  );
  const [firstEntry, setFirstEntry] = useState<number[]>([]);
  const [entry, setEntry] = useState<number[]>([]);
  const [codeBusy, setCodeBusy] = useState(false);
  const [codeResult, setCodeResult] = useState<string | null>(null);
  const [testResult, setTestResult] = useState<string | null>(null);

  /* The output config, purely to check an ignition channel exists. The
   * device validates that too, but LOCK_SET_CONFIG can only answer
   * REJECTED — one word for three different unmet requirements — so the
   * rider would be left guessing which. Checking here turns it into a
   * sentence that says what to go and fix. */
  const [outputs, setOutputs] = useState<DeviceConfig | null>(null);

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

  useEffect(() => {
    client
      .configRead()
      .then(setOutputs)
      .catch(() => {});
  }, [client]);

  /* Learn mode pushes every debounced press to the app (docs/PROTOCOL.md
   * §14.1). It must not outlive the capture — the device drops it on
   * disconnect too, so a failed call here is not fatal. */
  const capturing = capture !== 'idle';
  useEffect(() => {
    if (!capturing) return undefined;
    /* Suppressed: the rider is about to press whatever buttons make up
     * their code, and those buttons have jobs. */
    client.inputLearn(true, true).catch(() => {});
    const unsub = client.onInputEvent(event => {
      /* Short presses only, matching what the firmware feeds the matcher
       * (mc_lock.h) — a long press would be recorded here and then never
       * reproduce at the roadside. */
      if (event.pressType !== 'short') return;
      setEntry(prev =>
        prev.length >= CHEATCODE_MAX ? prev : [...prev, event.button],
      );
    });
    return () => {
      unsub();
      client.inputLearn(false).catch(() => {});
    };
  }, [client, capturing]);

  function beginCapture(): void {
    setCodeResult(null);
    setFirstEntry([]);
    setEntry([]);
    setCapture('first');
  }

  function cancelCapture(): void {
    setCapture('idle');
    setFirstEntry([]);
    setEntry([]);
  }

  function acceptEntry(): void {
    if (capture === 'test') {
      void testCheatcode(entry);
      return;
    }
    if (capture === 'first') {
      setFirstEntry(entry);
      setEntry([]);
      setCapture('second');
      return;
    }
    if (
      firstEntry.length !== entry.length ||
      firstEntry.some((b, i) => b !== entry[i])
    ) {
      setCodeResult("Those two didn't match. Start again.");
      cancelCapture();
      return;
    }
    void commitCheatcode(entry);
  }

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

  /** Mirrors mc_lock_config_validate(). Returns why the device would refuse
   * this config, or null if it wouldn't. */
  function whyRejected(): string | null {
    if (!config.immobilizerEnabled) return null;
    const hasIgnitionOutput = outputs?.outputs.channels.some(
      c => c.is_ignition,
    );
    if (outputs && !hasIgnitionOutput) {
      return 'No output is marked as the ignition, so there is nothing to immobilize. Set one under Settings → Outputs first.';
    }
    const hasSwitch =
      (config.methodsMask & LOCK_METHOD.IGNITION_SWITCH) !== 0 &&
      config.ignitionSwitchInput >= 0 &&
      config.ignitionSwitchInput < INPUT_COUNT;
    if (!config.cheatcodeSet && !hasSwitch) {
      return 'You need one way in that is not your phone: set a cheat-code below, or turn on Ignition switch and assign an input.';
    }
    return null;
  }

  async function saveConfig(): Promise<void> {
    const blocked = whyRejected();
    if (blocked) {
      setSaveResult(null);
      setError(blocked);
      return;
    }
    setSaving(true);
    setError(null);
    setSaveResult(null);
    try {
      const result = await client.lockSetConfig(config);
      if (result.ok) {
        setSaveResult('Saved.');
      } else {
        setSaveResult(
          `Rejected by device: ${result.resultName}. ${whyRejected() ?? 'Check that an ignition output is configured and one non-phone unlock method is set.'}`,
        );
      }
      await refreshConfig();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  async function commitCheatcode(buttons: number[]): Promise<void> {
    setCodeBusy(true);
    setCodeResult(null);
    try {
      const result = await client.cheatcodeSet(buttons);
      setCodeResult(
        result.ok ? 'Cheat-code set.' : `Rejected: ${result.resultName}`,
      );
      if (result.ok) {
        cancelCapture();
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
          : `Rejected: ${result.resultName} — with no ignition switch assigned, this code is the only way in besides your phone. Assign one, or disable the immobilizer first.`,
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

  async function testCheatcode(buttons: number[]): Promise<void> {
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
    } finally {
      cancelCapture();
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
      <SkeletonScreen title="Lock / Immobilizer" onBack={back} cards={4} />
    );
  }

  const methodPhone = (config.methodsMask & LOCK_METHOD.PHONE) !== 0;
  const methodIgnSwitch =
    (config.methodsMask & LOCK_METHOD.IGNITION_SWITCH) !== 0;
  /* Mirrors mc_lock_config_validate(): a method bit pointing at no input is
   * not a way in, so the switch only counts once one is assigned. */
  const hasNonPhoneFallback =
    config.cheatcodeSet ||
    (methodIgnSwitch &&
      config.ignitionSwitchInput >= 0 &&
      config.ignitionSwitchInput < INPUT_COUNT);

  function toggleMethod(bit: number, on: boolean): void {
    setConfig(prev => ({
      ...prev,
      methodsMask: on ? prev.methodsMask | bit : prev.methodsMask & ~bit,
    }));
  }

  return (
    <Screen
      title="Lock / Immobilizer"
      onBack={back}
      trailing={
        <Button
          label={saving ? 'Saving' : 'Save'}
          onPress={saveConfig}
          busy={saving}
          tone={dirty ? 'primary' : 'secondary'}
        />
      }
    >
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Immobilizer enabled</Text>
        <Switch
          value={config.immobilizerEnabled}
          onValueChange={v =>
            setConfig(prev => ({ ...prev, immobilizerEnabled: v }))
          }
        />
      </View>
      {/* layered unlock: the phone may never be the only way in. Either
       * fallback satisfies it, so a rider with an OEM key switch is not
       * forced to also set a code they will never use. */}
      {config.immobilizerEnabled && !hasNonPhoneFallback && (
        <Text style={styles.warn}>
          Set a cheat-code below, or assign an ignition switch, before turning
          this on. One way in that isn&apos;t your phone is required, so a flat
          battery can never strand you.
        </Text>
      )}
      {config.immobilizerEnabled && (
        <Text style={styles.hint}>
          While locked, nothing switches on — not the ignition, not the lights.
          Hazards are the exception, so a broken-down bike can be left flashing.
          Unlocking turns the ignition on, ready to start.
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
                label={buttonLabel(i, outputs)}
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
      {/* Both are tens of seconds in practice, so they read in seconds and
       * store milliseconds — see NumberField's `scale`. */}
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Auto-lock grace (seconds)</Text>
        <NumberInput
          style={styles.numInput}
          value={config.autoLockGraceMs}
          min={0}
          scale={1000}
          onChangeValue={v =>
            setConfig(prev => ({ ...prev, autoLockGraceMs: v }))
          }
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Cheat-code window (seconds)</Text>
        <NumberInput
          style={styles.numInput}
          value={config.cheatcodeWindowMs}
          min={0}
          scale={1000}
          onChangeValue={v =>
            setConfig(prev => ({ ...prev, cheatcodeWindowMs: v }))
          }
        />
      </View>

      {error && <Text style={styles.error}>{error}</Text>}
      {saveResult && <Text style={styles.success}>{saveResult}</Text>}

      <Text style={styles.sectionTitle}>Cheat-code</Text>
      <Text style={styles.hint}>
        {config.cheatcodeSet
          ? `Set (${config.cheatcodeLen} presses). Entered physically on the handlebar buttons while the bike is
             locked — this app never reads it back.`
          : 'Not set.'}
      </Text>
      {capturing ? (
        <>
          <Text style={styles.rowLabel}>
            {capture === 'test'
              ? 'Press the code you want to check'
              : capture === 'first'
                ? 'Press your code on the handlebar buttons'
                : 'Now press it again to confirm'}
          </Text>
          {/* Dots, not button numbers. The rider is watching their own hands;
           * printing which input each press mapped to would put the code on
           * screen for anyone stood beside the bike. */}
          <View style={styles.chipRow}>
            {entry.length === 0 ? (
              <Text style={styles.hint}>Waiting for the first press…</Text>
            ) : (
              entry.map((_, i) => <View key={i} style={styles.dot} />)
            )}
          </View>
          <Text style={styles.hint}>
            {entry.length} of {CHEATCODE_MIN}–{CHEATCODE_MAX} presses. Your
            handlebar controls are paused while you do this, so nothing switches
            on as you press.
          </Text>
          <View style={styles.actionRow}>
            <TouchableOpacity
              style={[
                styles.smallButton,
                (entry.length < CHEATCODE_MIN || codeBusy) && styles.disabled,
              ]}
              onPress={acceptEntry}
              disabled={entry.length < CHEATCODE_MIN || codeBusy}
            >
              <Text style={styles.smallButtonText}>
                {capture === 'test'
                  ? 'Check'
                  : capture === 'first'
                    ? 'Next'
                    : 'Save code'}
              </Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={styles.smallButton}
              onPress={() => setEntry([])}
              disabled={codeBusy}
            >
              <Text style={styles.smallButtonText}>Undo all</Text>
            </TouchableOpacity>
            <TouchableOpacity
              style={styles.smallButton}
              onPress={cancelCapture}
              disabled={codeBusy}
            >
              <Text style={styles.smallButtonText}>Cancel</Text>
            </TouchableOpacity>
          </View>
        </>
      ) : (
        <View style={styles.actionRow}>
          <TouchableOpacity
            style={[styles.smallButton, codeBusy && styles.disabled]}
            onPress={beginCapture}
            disabled={codeBusy}
          >
            <Text style={styles.smallButtonText}>
              {config.cheatcodeSet ? 'Change code' : 'Set code'}
            </Text>
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
      )}
      {codeResult && <Text style={styles.hint}>{codeResult}</Text>}

      <Text style={styles.sectionTitle}>Practise it (no side effects)</Text>
      <Text style={styles.hint}>
        Checks a sequence against the stored code without unlocking anything —
        worth doing once before you rely on it at the roadside.
      </Text>
      <TouchableOpacity
        style={[styles.smallButton, capturing && styles.disabled]}
        onPress={() => {
          setTestResult(null);
          setEntry([]);
          setCapture('test');
        }}
        disabled={capturing || !config.cheatcodeSet}
      >
        <Text style={styles.smallButtonText}>Practise</Text>
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
    </Screen>
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
  dot: {
    width: 12,
    height: 12,
    borderRadius: 6,
    backgroundColor: colors.accent,
  },
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
  disabled: { opacity: 0.5 },
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
