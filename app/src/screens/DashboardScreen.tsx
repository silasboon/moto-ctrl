/**
 * Dashboard: live status + output control (folded into one screen rather
 * than a separate "output control" screen — one set of 12 channel rows
 * with toggles covers both). Starter-function channels are shown disabled
 * — never offered as a live toggle — as defense in depth on top of the
 * server-side rejection in mc_output (AGENTS.md safety requirement #6):
 * don't even present a control that will always be refused.
 */
import React, { useEffect, useState } from 'react';
import { ScrollView, StyleSheet, Switch, Text, TouchableOpacity, View } from 'react-native';

import { LOCK_STATE, MC_LOCK_STATE, OUTPUT_COUNT } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import { isOutputOn, type DeviceConfig, type Status } from '../protocol/types';

interface Props {
  client: MotoClient;
  deviceName: string;
  onOpenPinMapper: () => void;
  onOpenKeys: () => void;
  onOpenLock: () => void;
  onOpenDiagnostics: () => void;
  onOpenFirmwareUpdate: () => void;
  onOpenEventLog: () => void;
  onDisconnect: () => void;
}

function Stat({ label, value }: { label: string; value: string }): React.JSX.Element {
  return (
    <View style={styles.stat}>
      <Text style={styles.statLabel}>{label}</Text>
      <Text style={styles.statValue}>{value}</Text>
    </View>
  );
}

export function DashboardScreen({
  client,
  deviceName,
  onOpenPinMapper,
  onOpenKeys,
  onOpenLock,
  onOpenDiagnostics,
  onOpenFirmwareUpdate,
  onOpenEventLog,
  onDisconnect,
}: Props): React.JSX.Element {
  const [status, setStatus] = useState<Status | null>(client.getLastStatus());
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [configError, setConfigError] = useState<string | null>(null);
  const [pending, setPending] = useState<Set<number>>(new Set());
  const [lockActionBusy, setLockActionBusy] = useState(false);
  const [lockActionError, setLockActionError] = useState<string | null>(null);
  const [hazardBusy, setHazardBusy] = useState(false);
  const [hazardError, setHazardError] = useState<string | null>(null);

  useEffect(() => {
    const unsub = client.onStatus(setStatus);
    client
      .configRead()
      .then(setConfig)
      .catch((err: unknown) => setConfigError(err instanceof Error ? err.message : String(err)));
    return unsub;
  }, [client]);

  async function toggle(channel: number, on: boolean): Promise<void> {
    setPending((prev) => new Set(prev).add(channel));
    try {
      await client.setOutput(channel, on);
    } catch {
      // Status poll will reflect the true device state regardless.
    } finally {
      setPending((prev) => {
        const next = new Set(prev);
        next.delete(channel);
        return next;
      });
    }
  }

  const lockLabel = status ? MC_LOCK_STATE[status.lockState as 0 | 1 | 2 | 3] ?? 'UNKNOWN' : '-';
  const isLocked = status?.lockState === LOCK_STATE.LOCKED;
  const hasTurnChannels =
    !!config?.outputs.channels.some((c) => c.function === 'turn_l') &&
    !!config?.outputs.channels.some((c) => c.function === 'turn_r');

  async function hazardPress(): Promise<void> {
    setHazardBusy(true);
    setHazardError(null);
    try {
      const result = await client.hazardPress();
      if (!result.ok) {
        setHazardError(`Hazard press rejected: ${result.resultName}`);
      }
    } catch (err) {
      setHazardError(err instanceof Error ? err.message : String(err));
    } finally {
      setHazardBusy(false);
    }
  }

  async function quickLockToggle(): Promise<void> {
    setLockActionBusy(true);
    setLockActionError(null);
    try {
      const result = isLocked ? await client.unlock() : await client.lock();
      if (!result.ok) {
        setLockActionError(`${isLocked ? 'Unlock' : 'Lock'} rejected: ${result.resultName}`);
      }
    } catch (err) {
      setLockActionError(err instanceof Error ? err.message : String(err));
    } finally {
      setLockActionBusy(false);
    }
  }

  return (
    <ScrollView style={styles.container} contentContainerStyle={styles.content}>
      <View style={styles.header}>
        <Text style={styles.deviceName}>{deviceName}</Text>
        <TouchableOpacity onPress={onDisconnect}>
          <Text style={styles.link}>Disconnect</Text>
        </TouchableOpacity>
      </View>

      <View style={styles.statGrid}>
        <Stat label="Lock" value={lockLabel} />
        <Stat
          label="Battery"
          value={status ? `${(status.batteryMv / 1000).toFixed(2)} V${status.lvCutoffActive ? ' ⚠' : ''}` : '-'}
        />
        <Stat label="Uptime" value={status ? `${Math.floor(status.uptimeMs / 1000)} s` : '-'} />
        <Stat label="Firmware" value={status ? `${status.fwMajor}.${status.fwMinor}.${status.fwPatch}` : '-'} />
      </View>
      {status?.lvCutoffActive && (
        <Text style={styles.warn}>
          Low-voltage cutoff active — non-essential outputs are suppressed until the battery recovers.
        </Text>
      )}
      {status && status.outputFaultMask !== 0 && (
        <Text style={styles.warn}>Output fault detected on at least one channel — see Diagnostics.</Text>
      )}

      {status && (status.lockState === LOCK_STATE.LOCKED || status.lockState === LOCK_STATE.PARKED) && (
        <View style={styles.lockActionRow}>
          <TouchableOpacity
            style={[styles.lockButton, isLocked ? styles.unlockButton : styles.lockButtonDanger, lockActionBusy && styles.disabled]}
            onPress={quickLockToggle}
            disabled={lockActionBusy || !client.isAuthenticated()}
          >
            <Text style={styles.lockButtonText}>
              {lockActionBusy ? 'Working…' : isLocked ? 'Unlock' : 'Lock now'}
            </Text>
          </TouchableOpacity>
          {status.cheatcodeBackoff && (
            <Text style={styles.warn}>Cheat-code in backoff — phone/ignition-switch unlock still work.</Text>
          )}
        </View>
      )}
      {lockActionError && <Text style={styles.error}>{lockActionError}</Text>}

      {hasTurnChannels && (
        <View style={styles.lockActionRow}>
          <TouchableOpacity
            style={[styles.lockButton, styles.hazardButton, hazardBusy && styles.disabled]}
            onPress={hazardPress}
            disabled={hazardBusy || !client.isAuthenticated()}
          >
            <Text style={styles.lockButtonText}>{hazardBusy ? 'Working…' : 'Hazards'}</Text>
          </TouchableOpacity>
        </View>
      )}
      {hazardError && <Text style={styles.error}>{hazardError}</Text>}

      <View style={styles.navRow}>
        <TouchableOpacity style={styles.navButton} onPress={onOpenPinMapper}>
          <Text style={styles.navButtonText}>Pin Mapper</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.navButton} onPress={onOpenKeys}>
          <Text style={styles.navButtonText}>Paired Keys</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.navButton} onPress={onOpenLock}>
          <Text style={styles.navButtonText}>Lock Settings</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.navButton} onPress={onOpenDiagnostics}>
          <Text style={styles.navButtonText}>Diagnostics</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.navButton} onPress={onOpenFirmwareUpdate}>
          <Text style={styles.navButtonText}>Firmware Update</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.navButton} onPress={onOpenEventLog}>
          <Text style={styles.navButtonText}>Event Log</Text>
        </TouchableOpacity>
      </View>

      <Text style={styles.sectionTitle}>Outputs</Text>
      {configError && <Text style={styles.error}>Config unavailable: {configError}</Text>}
      {Array.from({ length: OUTPUT_COUNT }).map((_, ch) => {
        const chCfg = config?.outputs.channels[ch];
        const isStarter = chCfg?.function === 'starter';
        const on = status ? isOutputOn(status, ch) : false;
        return (
          <View key={ch} style={styles.channelRow}>
            <View style={styles.channelInfo}>
              <Text style={styles.channelName}>{chCfg?.name || `Channel ${ch}`}</Text>
              <Text style={styles.channelFunc}>
                {chCfg?.function ?? '–'}
                {isStarter ? ' — hardware button only, never app-controllable' : ''}
              </Text>
            </View>
            <Switch
              value={on}
              disabled={isStarter || pending.has(ch) || !client.isAuthenticated()}
              onValueChange={(v) => toggle(ch, v)}
            />
          </View>
        );
      })}
    </ScrollView>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1 },
  content: { padding: 16, gap: 10 },
  header: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  deviceName: { fontSize: 20, fontWeight: '700' },
  link: { color: '#2563eb' },
  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: 8 },
  stat: { flexBasis: '48%', backgroundColor: '#f4f5f7', borderRadius: 8, padding: 10 },
  statLabel: { fontSize: 11, color: '#666', textTransform: 'uppercase' },
  statValue: { fontSize: 16, fontFamily: 'Menlo', marginTop: 2 },
  navRow: { flexDirection: 'row', gap: 8, marginTop: 4 },
  navButton: { flex: 1, padding: 10, borderRadius: 8, borderWidth: 1, borderColor: '#2563eb', alignItems: 'center' },
  navButtonText: { color: '#2563eb', fontWeight: '600' },
  lockActionRow: { gap: 6 },
  lockButton: { padding: 12, borderRadius: 8, alignItems: 'center' },
  unlockButton: { backgroundColor: '#15803d' },
  lockButtonDanger: { backgroundColor: '#b45309' },
  hazardButton: { backgroundColor: '#b91c1c' },
  lockButtonText: { color: 'white', fontWeight: '600' },
  disabled: { opacity: 0.5 },
  warn: { fontSize: 12, color: '#b45309' },
  sectionTitle: { fontSize: 13, color: '#666', textTransform: 'uppercase', marginTop: 8 },
  channelRow: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
    padding: 10,
    borderWidth: 1,
    borderColor: '#eee',
    borderRadius: 8,
  },
  channelInfo: { flex: 1, marginRight: 8 },
  channelName: { fontWeight: '600' },
  channelFunc: { fontSize: 11, color: '#888' },
  error: { color: '#b91c1c' },
});
