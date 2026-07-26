/**
 * Pin mapper: assign a function + friendly name to each of the 12 output
 * channels, pick the starter's neutral/clutch interlock input, and
 * each channel's mode (plain on/off, PWM dimming, turn-signal blink, or
 * brake-flasher pulse pattern) plus the flasher timing settings.
 *
 * No <Picker> — bare RN dropped it from core, and a community picker
 * package is another native dependency this environment can't verify
 * (see BlePlxTransport's header). Chip-style selectable buttons instead.
 *
 * `commanded_on` is intentionally not editable here — the device preserves
 * live on/off state on every commit regardless of what's imported
 * (mc_session.c's config_commit — AGENTS.md #1, importing config must never
 * toggle outputs), so exposing it as an editable field would be misleading.
 * `mode` IS editable — unlike commanded_on, the device applies
 * whatever mode is imported, since that's the whole point of a config edit
 * like this one.
 */
import React, { useEffect, useState } from 'react';
import { ActivityIndicator, ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';

import type { MotoClient } from '../protocol/MotoClient';
import { OUTPUT_FUNCTIONS, type DeviceConfig, type OutputFunction, type OutputMode } from '../protocol/types';

const OUTPUT_MODES: { value: OutputMode; label: string }[] = [
  { value: 'on', label: 'on/off' },
  { value: 'pwm', label: 'PWM dimmed' },
  { value: 'flash_turn', label: 'turn-signal blink' },
  { value: 'flash_brake', label: 'brake flasher' },
];

interface Props {
  client: MotoClient;
  onDone: () => void;
}

function Chip({ label, active, onPress }: { label: string; active: boolean; onPress: () => void }): React.JSX.Element {
  return (
    <TouchableOpacity style={[styles.chip, active && styles.chipActive]} onPress={onPress}>
      <Text style={active ? styles.chipTextActive : styles.chipText}>{label}</Text>
    </TouchableOpacity>
  );
}

export function PinMapperScreen({ client, onDone }: Props): React.JSX.Element {
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saveResult, setSaveResult] = useState<string | null>(null);

  useEffect(() => {
    client
      .configRead()
      .then((c) => setConfig(c))
      .catch((err: unknown) => setError(err instanceof Error ? err.message : String(err)))
      .finally(() => setLoading(false));
  }, [client]);

  function setFunction(ch: number, fn: OutputFunction): void {
    setConfig((prev) => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.slice();
      channels[ch] = { ...channels[ch]!, function: fn };
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
  }

  function setName(ch: number, name: string): void {
    setConfig((prev) => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.slice();
      channels[ch] = { ...channels[ch]!, name };
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
  }

  function setMode(ch: number, mode: OutputMode): void {
    setConfig((prev) => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.slice();
      channels[ch] = { ...channels[ch]!, mode };
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
  }

  function setDuty(ch: number, pct: number): void {
    setConfig((prev) => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.slice();
      channels[ch] = { ...channels[ch]!, pwm_duty_pct: pct };
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
  }

  function setInterlock(input: number): void {
    setConfig((prev) => (prev ? { ...prev, outputs: { ...prev.outputs, starter_interlock_input: input } } : prev));
  }

  function setBrakeSwitchInput(input: number): void {
    setConfig((prev) => (prev ? { ...prev, outputs: { ...prev.outputs, brake_switch_input: input } } : prev));
  }

  function setFlasherField<K extends 'turn_auto_cancel_ms' | 'turn_flash_period_ms' | 'brake_flash_pulse_count' | 'brake_flash_pulse_on_ms' | 'brake_flash_pulse_off_ms'>(
    field: K,
    value: number,
  ): void {
    setConfig((prev) => (prev ? { ...prev, outputs: { ...prev.outputs, [field]: value } } : prev));
  }

  async function save(): Promise<void> {
    if (!config) return;
    // Mirrors mc_output_config_validate's rules client-side, for a fast
    // error — the device re-validates and is the actual authority.
    const ignitionCount = config.outputs.channels.filter((c) => c.function === 'ignition').length;
    const starterCount = config.outputs.channels.filter((c) => c.function === 'starter').length;
    if (ignitionCount > 1) {
      setError('At most one channel may be assigned the ignition function.');
      return;
    }
    if (starterCount > 1) {
      setError('At most one channel may be assigned the starter function.');
      return;
    }
    setSaving(true);
    setError(null);
    setSaveResult(null);
    try {
      const result = await client.configWrite(config);
      setSaveResult(result.ok ? 'Saved.' : `Rejected by device: ${result.resultName}`);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  if (loading) {
    return (
      <View style={styles.center}>
        <ActivityIndicator />
      </View>
    );
  }
  if (!config) {
    return (
      <View style={styles.center}>
        <Text style={styles.error}>{error ?? 'Could not read config.'}</Text>
        <TouchableOpacity onPress={onDone}>
          <Text style={styles.link}>Back</Text>
        </TouchableOpacity>
      </View>
    );
  }

  return (
    <ScrollView style={styles.container} contentContainerStyle={styles.content}>
      <View style={styles.header}>
        <Text style={styles.title}>Pin Mapper</Text>
        <TouchableOpacity onPress={onDone}>
          <Text style={styles.link}>Back</Text>
        </TouchableOpacity>
      </View>

      {config.outputs.channels.map((ch, i) => (
        <View key={i} style={styles.channelBlock}>
          <Text style={styles.channelIndex}>CH {i}</Text>
          <TextInput
            style={styles.nameInput}
            value={ch.name}
            onChangeText={(v) => setName(i, v)}
            placeholder={`Channel ${i}`}
          />
          <View style={styles.chipRow}>
            {OUTPUT_FUNCTIONS.map((fn) => (
              <Chip key={fn} label={fn} active={ch.function === fn} onPress={() => setFunction(i, fn)} />
            ))}
          </View>
          <View style={styles.chipRow}>
            {OUTPUT_MODES.map((m) => (
              <Chip key={m.value} label={m.label} active={ch.mode === m.value} onPress={() => setMode(i, m.value)} />
            ))}
          </View>
          {ch.mode === 'pwm' && (
            <View style={styles.dutyRow}>
              <Text style={styles.dutyLabel}>Duty</Text>
              <TextInput
                style={styles.dutyInput}
                keyboardType="number-pad"
                value={String(ch.pwm_duty_pct)}
                onChangeText={(v) => setDuty(i, Math.max(1, Math.min(100, parseInt(v, 10) || 1)))}
              />
              <Text style={styles.dutyLabel}>%</Text>
            </View>
          )}
        </View>
      ))}

      <Text style={styles.sectionTitle}>Starter interlock input</Text>
      <View style={styles.chipRow}>
        <Chip
          label="none"
          active={config.outputs.starter_interlock_input === -1}
          onPress={() => setInterlock(-1)}
        />
        {Array.from({ length: 8 }).map((_, i) => (
          <Chip
            key={i}
            label={`input ${i}`}
            active={config.outputs.starter_interlock_input === i}
            onPress={() => setInterlock(i)}
          />
        ))}
      </View>

      <Text style={styles.sectionTitle}>Brake switch input</Text>
      <Text style={styles.hint}>
        A maintained switch (not a press), read as a level — assign the input the brake lever/pedal switch is wired
        to. Drives the first "brake"-function channel above directly.
      </Text>
      <View style={styles.chipRow}>
        <Chip label="none" active={config.outputs.brake_switch_input === -1} onPress={() => setBrakeSwitchInput(-1)} />
        {Array.from({ length: 8 }).map((_, i) => (
          <Chip
            key={i}
            label={`input ${i}`}
            active={config.outputs.brake_switch_input === i}
            onPress={() => setBrakeSwitchInput(i)}
          />
        ))}
      </View>

      <Text style={styles.sectionTitle}>Flasher timing</Text>
      <View style={styles.timingRow}>
        <Text style={styles.timingLabel}>Turn auto-cancel</Text>
        <TextInput
          style={styles.timingInput}
          keyboardType="number-pad"
          value={String(config.outputs.turn_auto_cancel_ms)}
          onChangeText={(v) => setFlasherField('turn_auto_cancel_ms', Math.max(0, parseInt(v, 10) || 0))}
        />
        <Text style={styles.timingLabel}>ms (0 = never)</Text>
      </View>
      <View style={styles.timingRow}>
        <Text style={styles.timingLabel}>Turn blink period</Text>
        <TextInput
          style={styles.timingInput}
          keyboardType="number-pad"
          value={String(config.outputs.turn_flash_period_ms)}
          onChangeText={(v) => setFlasherField('turn_flash_period_ms', Math.max(1, parseInt(v, 10) || 1))}
        />
        <Text style={styles.timingLabel}>ms</Text>
      </View>
      <View style={styles.timingRow}>
        <Text style={styles.timingLabel}>Brake pulses</Text>
        <TextInput
          style={styles.timingInput}
          keyboardType="number-pad"
          value={String(config.outputs.brake_flash_pulse_count)}
          onChangeText={(v) => setFlasherField('brake_flash_pulse_count', Math.max(0, parseInt(v, 10) || 0))}
        />
        <Text style={styles.timingLabel}>on</Text>
        <TextInput
          style={styles.timingInput}
          keyboardType="number-pad"
          value={String(config.outputs.brake_flash_pulse_on_ms)}
          onChangeText={(v) => setFlasherField('brake_flash_pulse_on_ms', Math.max(0, parseInt(v, 10) || 0))}
        />
        <Text style={styles.timingLabel}>ms / off</Text>
        <TextInput
          style={styles.timingInput}
          keyboardType="number-pad"
          value={String(config.outputs.brake_flash_pulse_off_ms)}
          onChangeText={(v) => setFlasherField('brake_flash_pulse_off_ms', Math.max(0, parseInt(v, 10) || 0))}
        />
        <Text style={styles.timingLabel}>ms</Text>
      </View>
      <Text style={styles.hint}>
        Brake flash patterns are not legal in all jurisdictions — the "brake flasher" mode is opt-in per channel, not
        the default, for exactly this reason.
      </Text>

      {error && <Text style={styles.error}>{error}</Text>}
      {saveResult && <Text style={styles.success}>{saveResult}</Text>}

      <TouchableOpacity style={[styles.saveButton, saving && styles.disabled]} onPress={save} disabled={saving}>
        <Text style={styles.saveButtonText}>{saving ? 'Saving…' : 'Save'}</Text>
      </TouchableOpacity>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  content: { padding: 16, gap: 12 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 8 },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  title: { fontSize: 20, fontWeight: '700' },
  link: { color: '#2563eb' },
  channelBlock: { borderWidth: 1, borderColor: '#eee', borderRadius: 8, padding: 10, gap: 6 },
  channelIndex: { fontSize: 11, color: '#888', fontFamily: 'Menlo' },
  nameInput: { borderWidth: 1, borderColor: '#ccc', borderRadius: 6, padding: 8 },
  chipRow: { flexDirection: 'row', flexWrap: 'wrap', gap: 6 },
  chip: { paddingVertical: 6, paddingHorizontal: 10, borderRadius: 999, borderWidth: 1, borderColor: '#ccc' },
  chipActive: { backgroundColor: '#2563eb', borderColor: '#2563eb' },
  chipText: { fontSize: 12, color: '#333' },
  chipTextActive: { fontSize: 12, color: 'white', fontWeight: '600' },
  sectionTitle: { fontSize: 13, color: '#666', textTransform: 'uppercase', marginTop: 8 },
  hint: { fontSize: 12, color: '#888' },
  dutyRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  dutyLabel: { fontSize: 12, color: '#666' },
  dutyInput: { borderWidth: 1, borderColor: '#ccc', borderRadius: 6, padding: 6, width: 56, fontFamily: 'Menlo' },
  timingRow: { flexDirection: 'row', alignItems: 'center', gap: 6 },
  timingLabel: { fontSize: 12, color: '#666' },
  timingInput: { borderWidth: 1, borderColor: '#ccc', borderRadius: 6, padding: 6, width: 64, fontFamily: 'Menlo' },
  error: { color: '#b91c1c' },
  success: { color: '#15803d' },
  saveButton: { backgroundColor: '#2563eb', padding: 12, borderRadius: 8, alignItems: 'center', marginTop: 8 },
  disabled: { opacity: 0.5 },
  saveButtonText: { color: 'white', fontWeight: '600' },
});
