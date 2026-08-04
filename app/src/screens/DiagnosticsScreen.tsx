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
import { StyleSheet, Text, TouchableOpacity, View } from 'react-native';

import {
  DIAG_FAULT,
  DIAG_LEARN_ALL,
  OUTPUT_COUNT,
} from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import {
  NumberInput,
  Screen,
  SkeletonScreen,
  useLeaveGuard,
} from '../ui/components';
import { colors } from '../ui/theme';
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

export function DiagnosticsScreen({
  client,
  onDone,
}: Props): React.JSX.Element {
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
  /* Serialised copies of what the board last told us. Thresholds and
   * calibration save separately, so each needs its own baseline — see
   * OutputsScreen for why this is a string rather than an object. */
  const [diagBaseline, setDiagBaseline] = useState<string | null>(null);
  const [calibBaseline, setCalibBaseline] = useState<string | null>(null);

  useEffect(() => {
    Promise.all([
      client.diagGetConfig(),
      client.diagGetCalib(),
      client.configRead(),
    ])
      .then(([cfg, cal, deviceCfg]) => {
        setDiagConfig(cfg);
        setCalib(cal);
        setConfig(deviceCfg);
        setDiagBaseline(JSON.stringify(cfg));
        setCalibBaseline(JSON.stringify(cal));
      })
      .catch((err: unknown) =>
        setError(err instanceof Error ? err.message : String(err)),
      )
      .finally(() => setLoading(false));
  }, [client]);

  useEffect(() => {
    let cancelled = false;
    const poll = () => {
      client
        .getDiagnostics()
        .then(d => {
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
      const fresh = await client.diagGetConfig();
      setDiagConfig(fresh);
      setDiagBaseline(JSON.stringify(fresh));
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
      setSaveResult(
        result.ok ? 'Saved.' : `Rejected by device: ${result.resultName}`,
      );
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
      setCalibResult(
        result.ok ? 'Saved.' : `Rejected by device: ${result.resultName}`,
      );
      if (result.ok) {
        const fresh = await client.diagGetCalib();
        setCalib(fresh);
        setCalibBaseline(JSON.stringify(fresh));
      }
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

  function setChannelField(
    ch: number,
    field: 'openLoadMa' | 'overcurrentMa',
    value: number,
  ): void {
    setDiagConfig(prev => {
      const channels = prev.channels.slice();
      channels[ch] = { ...channels[ch]!, [field]: value };
      return { ...prev, channels };
    });
  }

  const dirty =
    diagBaseline !== null &&
    (JSON.stringify(diagConfig) !== diagBaseline ||
      JSON.stringify(calib) !== calibBaseline);
  const back = useLeaveGuard(dirty, onDone);

  if (loading) {
    return <SkeletonScreen title="Diagnostics" onBack={back} cards={4} />;
  }

  return (
    <Screen title="Diagnostics" onBack={back}>
      <Text style={styles.sectionTitle}>Battery / cutoff</Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Low-voltage cutoff (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={diagConfig.lvCutoffMv}
          min={0}
          onChangeValue={v =>
            setDiagConfig(prev => ({ ...prev, lvCutoffMv: v }))
          }
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Cutoff hysteresis (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={diagConfig.lvCutoffHysteresisMv}
          min={0}
          onChangeValue={v =>
            setDiagConfig(prev => ({ ...prev, lvCutoffHysteresisMv: v }))
          }
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Engine-running threshold (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={diagConfig.engineRunMv}
          min={0}
          onChangeValue={v =>
            setDiagConfig(prev => ({ ...prev, engineRunMv: v }))
          }
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Engine-run hysteresis (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={diagConfig.engineRunHysteresisMv}
          min={0}
          onChangeValue={v =>
            setDiagConfig(prev => ({ ...prev, engineRunHysteresisMv: v }))
          }
        />
      </View>
      <Text style={styles.hint}>
        Below the cutoff (and only while the engine isn&apos;t detected
        running), non-essential outputs are suppressed automatically — ignition,
        brake, and both headlight channels are never affected.
      </Text>

      {error && <Text style={styles.error}>{error}</Text>}
      {saveResult && <Text style={styles.success}>{saveResult}</Text>}
      <TouchableOpacity
        style={[styles.saveButton, saving && styles.disabled]}
        onPress={saveConfig}
        disabled={saving}
      >
        <Text style={styles.saveButtonText}>{saving ? 'Saving…' : 'Save'}</Text>
      </TouchableOpacity>

      <View style={styles.channelsHeaderRow}>
        <Text style={styles.sectionTitle}>Per-channel current sense</Text>
        <TouchableOpacity
          style={[styles.smallButton, learnBusy === -1 && styles.disabled]}
          onPress={() => learn(DIAG_LEARN_ALL)}
          disabled={learnBusy !== null}
        >
          <Text style={styles.smallButtonText}>
            {learnBusy === -1 ? 'Learning…' : 'Learn all energized'}
          </Text>
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
              <Text style={styles.channelName}>
                {chCfg?.name || `Channel ${ch}`}
              </Text>
              <Text style={[styles.readingText, hasFault && styles.error]}>
                {reading ? `${reading.currentMa} mA` : '– mA'}
                {reading && hasFault
                  ? ` · ${FAULT_LABEL[reading.fault] ?? reading.fault}`
                  : ''}
              </Text>
            </View>
            <View style={styles.row}>
              <Text style={styles.rowLabelSmall}>Open-load (mA)</Text>
              <NumberInput
                style={styles.numInputSmall}
                value={diagConfig.channels[ch]?.openLoadMa ?? 0}
                min={0}
                onChangeValue={v => setChannelField(ch, 'openLoadMa', v)}
              />
              <Text style={styles.rowLabelSmall}>Overcurrent (mA)</Text>
              <NumberInput
                style={styles.numInputSmall}
                value={diagConfig.channels[ch]?.overcurrentMa ?? 0}
                min={0}
                onChangeValue={v => setChannelField(ch, 'overcurrentMa', v)}
              />
              <TouchableOpacity
                style={[
                  styles.smallButton,
                  learnBusy === ch && styles.disabled,
                ]}
                onPress={() => learn(ch)}
                disabled={learnBusy !== null}
              >
                <Text style={styles.smallButtonText}>
                  {learnBusy === ch ? '…' : 'Learn'}
                </Text>
              </TouchableOpacity>
            </View>
          </View>
        );
      })}

      <Text style={styles.sectionTitle}>Board calibration</Text>
      <Text style={styles.hint}>
        Bench/installer values for this specific board&apos;s sense lines —
        never included in a config export/import, and not wiped by ownership
        transfer (it describes the board, not the owner).
      </Text>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>IS gain</Text>
        <NumberInput
          style={styles.numInput}
          value={calib.isGain}
          decimal
          min={0}
          onChangeValue={v => setCalib(prev => ({ ...prev, isGain: v }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>IS offset (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={calib.isOffsetMv}
          onChangeValue={v => setCalib(prev => ({ ...prev, isOffsetMv: v }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>kILIS</Text>
        <NumberInput
          style={styles.numInput}
          value={calib.kilis}
          decimal
          min={0}
          onChangeValue={v => setCalib(prev => ({ ...prev, kilis: v }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Vbat gain</Text>
        <NumberInput
          style={styles.numInput}
          value={calib.vbatGain}
          decimal
          min={0}
          onChangeValue={v => setCalib(prev => ({ ...prev, vbatGain: v }))}
        />
      </View>
      <View style={styles.row}>
        <Text style={styles.rowLabel}>Vbat offset (mV)</Text>
        <NumberInput
          style={styles.numInput}
          value={calib.vbatOffsetMv}
          onChangeValue={v => setCalib(prev => ({ ...prev, vbatOffsetMv: v }))}
        />
      </View>
      {calibResult && <Text style={styles.hint}>{calibResult}</Text>}
      <TouchableOpacity
        style={[styles.saveButton, calibSaving && styles.disabled]}
        onPress={saveCalib}
        disabled={calibSaving}
      >
        <Text style={styles.saveButtonText}>
          {calibSaving ? 'Saving…' : 'Save calibration'}
        </Text>
      </TouchableOpacity>
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
    gap: 6,
  },
  rowLabel: { color: colors.text, fontSize: 14 },
  rowLabelSmall: { fontSize: 11, color: colors.textMuted },
  sectionTitle: {
    fontSize: 13,
    color: colors.textMuted,
    textTransform: 'uppercase',
    marginTop: 10,
  },
  hint: { fontSize: 12, color: colors.textFaint },
  numInput: {
    borderWidth: 1,
    borderColor: colors.borderStrong,
    borderRadius: 6,
    padding: 8,
    width: 90,
    textAlign: 'right',
    color: colors.text,
  },
  numInputSmall: {
    borderWidth: 1,
    borderColor: colors.borderStrong,
    borderRadius: 6,
    padding: 6,
    width: 64,
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
  channelsHeaderRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    marginTop: 10,
  },
  channelCard: {
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: 8,
    padding: 10,
    gap: 4,
  },
  channelHeaderRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  channelName: { color: colors.text, fontWeight: '600' },
  readingText: { color: colors.text, fontFamily: 'Menlo', fontSize: 12 },
  smallButton: {
    paddingVertical: 6,
    paddingHorizontal: 10,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: colors.accent,
    alignItems: 'center',
  },
  smallButtonText: { color: colors.accent, fontWeight: '600', fontSize: 12 },
});
