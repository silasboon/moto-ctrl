/**
 * Diagnostics (docs/PROTOCOL.md §12): per-channel current-sense
 * readout + open-load/overcurrent thresholds (with a "Learn" action per
 * channel), the low-voltage cutoff and engine-running voltage thresholds,
 * and board calibration.
 *
 * Follows LockScreen's read-edit-save pattern for the config half (a
 * dedicated wire op, DIAG_GET_CONFIG/DIAG_SET_CONFIG — not the generic
 * config JSON, same reasoning as lock config). Live current/fault readings
 * (DIAG_GET) are polled on an interval while this screen is open, since
 * they change on their own (round-robin sampling) rather than only when the
 * user edits something.
 *
 * Calibration is a separate, less-frequently-touched section: it's a
 * bench/installer action, not a routine one, and never rides a config
 * export/import or ownership transfer (see DiagCalib's doc comment).
 *
 * Fetches its own DeviceConfig (for channel names only, never edited here)
 * rather than receiving it as a prop — same independent-fetch pattern
 * DashboardScreen/LockScreen/PinMapperScreen already each follow.
 */
import React, { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Text, TextInput, TouchableOpacity, View } from 'react-native';

import { DIAG_FAULT, DIAG_LEARN_ALL, OUTPUT_COUNT } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import {
  defaultDiagCalib,
  defaultDiagConfig,
  type DeviceConfig,
  type DiagCalib,
  type DiagConfig,
  type Diagnostics,
} from '../protocol/types';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

const DIAG_POLL_MS = 1500;

const FAULT_LABEL: Record<number, string> = {
  [DIAG_FAULT.NONE]: 'none',
  [DIAG_FAULT.OPEN_LOAD]: 'OPEN LOAD',
  [DIAG_FAULT.OVERCURRENT]: 'OVERCURRENT',
};

export function DiagnosticsScreen({ client, onDone }: Props): React.JSX.Element {
  const [diagConfig, setDiagConfig] = useState<DiagConfig>(defaultDiagConfig());
  const [live, setLive] = useState<Diagnostics | null>(null);
  const [calib, setCalib] = useState<DiagCalib>(defaultDiagCalib());
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [saveResult, setSaveResult] = useState<string | null>(null);
  const [calibSaving, setCalibSaving] = useState(false);
  const [calibResult, setCalibResult] = useState<string | null>(null);
  const [learnBusy, setLearnBusy] = useState<number | null>(null); // channel index, or -1 for "all"
  const [learnResult, setLearnResult] = useState<string | null>(null);

  useEffect(() => {
    Promise.all([client.diagGetConfig(), client.diagGetCalib(), client.configRead()])
      .then(([cfg, cal, deviceCfg]) => {
        setDiagConfig(cfg);
        setCalib(cal);
        setConfig(deviceCfg);
      })
      .catch((err: unknown) => setError(err instanceof Error ? err.message : String(err)))
      .finally(() => setLoading(false));
  }, [client]);

  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      client
        .getDiagnostics()
        .then((d) => {
          if (!cancelled) setLive(d);
        })
        .catch(() => {
          // Live readout is best-effort; keep showing the last-known values.
        });
    };
    poll();
    const timer = setInterval(poll, DIAG_POLL_MS);
    return () => {
      cancelled = true;
      clearInterval(timer);
    };
  }, [client]);

  async function refreshConfig(): Promise<void> {
    try {
      setDiagConfig(await client.diagGetConfig());
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
      const result = await client.diagSetConfig(diagConfig);
      setSaveResult(result.ok ? 'Saved.' : `Rejected by device: ${result.resultName}`);
      await refreshConfig();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  async function saveCalib(): Promise<void> {
    setCalibSaving(true);
    setCalibResult(null);
    try {
      const result = await client.diagSetCalib(calib);
      setCalibResult(result.ok ? 'Saved.' : `Rejected by device: ${result.resultName}`);
      if (result.ok) setCalib(await client.diagGetCalib());
    } catch (err) {
      setCalibResult(err instanceof Error ? err.message : String(err));
    } finally {
      setCalibSaving(false);
    }
  }

  async function learn(channel: number): Promise<void> {
    setLearnBusy(channel === DIAG_LEARN_ALL ? -1 : channel);
    setLearnResult(null);
    try {
      const result = await client.diagLearn(channel);
      setLearnResult(
        result.ok
          ? channel === DIAG_LEARN_ALL
            ? 'Learned every energized channel.'
            : `Learned channel ${channel}.`
          : `Rejected: ${result.resultName} — the channel must be ON to learn from it.`,
      );
      if (result.ok) await refreshConfig();
    } catch (err) {
      setLearnResult(err instanceof Error ? err.message : String(err));
    } finally {
      setLearnBusy(null);
    }
  }

  function setChannelField(ch: number, field: 'openLoadMa' | 'overcurrentMa', value: number): void {
    setDiagConfig((prev) => {
      const channels = prev.channels.slice();
      channels[ch] = { ...channels[ch]!, [field]: value };
      return { ...prev, channels };
    });
  }

  if (loading) {
    return (
      <View style={styles.center}>
        <Text>Loading…</Text>
      </View>
    );
  }

  return (
    <ScrollView style={styles.container} contentContainerStyle={styles.content}>
      <View style={styles.header}>
        <Text style={styles.title}>Diagnostics</Text>
        <TouchableOpacity onPress={onDone}>
          <Text style={styles.link}>Back</Text>
        </TouchableOpacity>
      </View>

      <Text style={styles.sectionTitle}>Battery / cutoff</Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Low-voltage cutoff (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(diagConfig.lvCutoffMv)}
          onChangeText={(v) => setDiagConfig((prev) => ({ ...prev, lvCutoffMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Cutoff hysteresis (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(diagConfig.lvCutoffHysteresisMv)}
          onChangeText={(v) => setDiagConfig((prev) => ({ ...prev, lvCutoffHysteresisMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Engine-running threshold (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(diagConfig.engineRunMv)}
          onChangeText={(v) => setDiagConfig((prev) => ({ ...prev, engineRunMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Engine-run hysteresis (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(diagConfig.engineRunHysteresisMv)}
          onChangeText={(v) => setDiagConfig((prev) => ({ ...prev, engineRunHysteresisMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      <Text style={styles.hint}>
        Below the cutoff (and only while the engine isn&apos;t detected running), non-essential outputs are
        suppressed automatically — ignition, brake, and both headlight channels are never affected.
      </Text>

      {error && <Text style={styles.error}>{error}</Text>}
      {saveResult && <Text style={styles.success}>{saveResult}</Text>}
      <TouchableOpacity style={[styles.saveButton, saving && styles.disabled]} onPress={saveConfig} disabled={saving}>
        <Text style={styles.saveButtonText}>{saving ? 'Saving…' : 'Save'}</Text>
      </TouchableOpacity>

      <View style={styles.channelsHeaderRow}>
        <Text style={styles.sectionTitle}>Per-channel current sense</Text>
        <TouchableOpacity
          style={[styles.smallButton, learnBusy === -1 && styles.disabled]}
          onPress={() => learn(DIAG_LEARN_ALL)}
          disabled={learnBusy !== null}
        >
          <Text style={styles.smallButtonText}>{learnBusy === -1 ? 'Learning…' : 'Learn all energized'}</Text>
        </TouchableOpacity>
      </View>
      {learnResult && <Text style={styles.hint}>{learnResult}</Text>}

      {Array.from({ length: OUTPUT_COUNT }).map((_, ch) => {
        const chCfg = config?.outputs.channels[ch];
        const reading = live?.channels[ch];
        const hasFault = !!reading && reading.fault !== DIAG_FAULT.NONE;
        return (
          <View key={ch} style={styles.channelCard}>
            <View style={styles.channelHeaderRow}>
              <Text style={styles.channelName}>{chCfg?.name || `Channel ${ch}`}</Text>
              <Text style={[styles.readingText, hasFault && styles.error]}>
                {reading ? `${reading.currentMa} mA` : '– mA'}
                {reading && hasFault ? ` · ${FAULT_LABEL[reading.fault] ?? reading.fault}` : ''}
              </Text>
            </View>
            <View style={styles.row}>
              <Text style={styles.rowLabelSmall}>Open-load (mA)</Text>
              <TextInput
                style={styles.numInputSmall}
                keyboardType="number-pad"
                value={String(diagConfig.channels[ch]?.openLoadMa ?? 0)}
                onChangeText={(v) => setChannelField(ch, 'openLoadMa', parseInt(v, 10) || 0)}
              />
              <Text style={styles.rowLabelSmall}>Overcurrent (mA)</Text>
              <TextInput
                style={styles.numInputSmall}
                keyboardType="number-pad"
                value={String(diagConfig.channels[ch]?.overcurrentMa ?? 0)}
                onChangeText={(v) => setChannelField(ch, 'overcurrentMa', parseInt(v, 10) || 0)}
              />
              <TouchableOpacity
                style={[styles.smallButton, learnBusy === ch && styles.disabled]}
                onPress={() => learn(ch)}
                disabled={learnBusy !== null}
              >
                <Text style={styles.smallButtonText}>{learnBusy === ch ? '…' : 'Learn'}</Text>
              </TouchableOpacity>
            </View>
          </View>
        );
      })}

      <Text style={styles.sectionTitle}>Board calibration</Text>
      <Text style={styles.hint}>
        Bench/installer values for this specific board&apos;s sense lines — never included in a config
        export/import, and not wiped by ownership transfer (it describes the board, not the owner).
      </Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>IS gain</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="numeric"
          value={String(calib.isGain)}
          onChangeText={(v) => setCalib((prev) => ({ ...prev, isGain: parseFloat(v) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>IS offset (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(calib.isOffsetMv)}
          onChangeText={(v) => setCalib((prev) => ({ ...prev, isOffsetMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>kILIS</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="numeric"
          value={String(calib.kilis)}
          onChangeText={(v) => setCalib((prev) => ({ ...prev, kilis: parseFloat(v) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Vbat gain</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="numeric"
          value={String(calib.vbatGain)}
          onChangeText={(v) => setCalib((prev) => ({ ...prev, vbatGain: parseFloat(v) || 0 }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Vbat offset (mV)</Text>
        <TextInput
          style={styles.numInput}
          keyboardType="number-pad"
          value={String(calib.vbatOffsetMv)}
          onChangeText={(v) => setCalib((prev) => ({ ...prev, vbatOffsetMv: parseInt(v, 10) || 0 }))}
        />
      </View>
      {calibResult && <Text style={styles.hint}>{calibResult}</Text>}
      <TouchableOpacity
        style={[styles.saveButton, calibSaving && styles.disabled]}
        onPress={saveCalib}
        disabled={calibSaving}
      >
        <Text style={styles.saveButtonText}>{calibSaving ? 'Saving…' : 'Save calibration'}</Text>
      </TouchableOpacity>
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  content: { padding: 16, gap: 10 },
  center: { flex: 1, alignItems: 'center', justifyContent: 'center' },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  title: { fontSize: 20, fontWeight: '700' },
  link: { color: '#2563eb' },
  row: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', paddingVertical: 4, gap: 6 },
  rowLabel: { fontSize: 14 },
  rowLabelSmall: { fontSize: 11, color: '#666' },
  sectionTitle: { fontSize: 13, color: '#666', textTransform: 'uppercase', marginTop: 10 },
  hint: { fontSize: 12, color: '#888' },
  numInput: { borderWidth: 1, borderColor: '#ccc', borderRadius: 6, padding: 8, width: 90, textAlign: 'right' },
  numInputSmall: { borderWidth: 1, borderColor: '#ccc', borderRadius: 6, padding: 6, width: 64, textAlign: 'right' },
  error: { color: '#b91c1c' },
  success: { color: '#15803d' },
  saveButton: { backgroundColor: '#2563eb', padding: 12, borderRadius: 8, alignItems: 'center', marginTop: 4 },
  disabled: { opacity: 0.5 },
  saveButtonText: { color: 'white', fontWeight: '600' },
  channelsHeaderRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginTop: 10 },
  channelCard: { borderWidth: 1, borderColor: '#eee', borderRadius: 8, padding: 10, gap: 4 },
  channelHeaderRow: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  channelName: { fontWeight: '600' },
  readingText: { fontFamily: 'Menlo', fontSize: 12 },
  smallButton: {
    paddingVertical: 6,
    paddingHorizontal: 10,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: '#2563eb',
    alignItems: 'center',
  },
  smallButtonText: { color: '#2563eb', fontWeight: '600', fontSize: 12 },
});
