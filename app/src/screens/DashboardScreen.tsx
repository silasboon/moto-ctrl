/**
 * Dashboard: live status, quick actions, and output control folded into one
 * screen — one set of 12 channel rows with toggles covers both status and
 * control, so there's no separate "output control" screen.
 *
 * Starter-function channels are shown as locked out rather than as a toggle
 * that will always be refused: mc_output rejects a remote starter command
 * outright (AGENTS.md #6), and presenting a dead switch is worse than
 * presenting none. Same reasoning for hiding the lock button unless the bike
 * is actually in a state that can be locked.
 */
import React, { useEffect, useState } from 'react';
import { Pressable, StyleSheet, Switch, Text, View } from 'react-native';

import { LOCK_STATE, MC_LOCK_STATE, OUTPUT_COUNT } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import { isOutputOn, type DeviceConfig, type Status } from '../protocol/types';
import {
  Badge,
  Button,
  Card,
  Divider,
  Notice,
  Screen,
  SectionHeader,
  Stat,
} from '../ui/components';
import { colors, radius, space, type } from '../ui/theme';

interface Props {
  client: MotoClient;
  deviceName: string;
  onOpenOutputs: () => void;
  onOpenButtons: () => void;
  onOpenKeys: () => void;
  onOpenLock: () => void;
  onOpenDiagnostics: () => void;
  onOpenFirmwareUpdate: () => void;
  onOpenEventLog: () => void;
  onDisconnect: () => void;
}

/** Navigation tile. Two per row, so labels stay readable at any font size. */
function NavTile({
  label,
  detail,
  onPress,
}: {
  label: string;
  detail: string;
  onPress: () => void;
}): React.JSX.Element {
  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityLabel={`${label}. ${detail}`}
      style={({ pressed }) => [
        styles.navTile,
        pressed && styles.navTilePressed,
      ]}
    >
      <Text style={styles.navLabel}>{label}</Text>
      <Text style={type.caption} numberOfLines={2}>
        {detail}
      </Text>
    </Pressable>
  );
}

export function DashboardScreen({
  client,
  deviceName,
  onOpenOutputs,
  onOpenButtons,
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
      .catch((err: unknown) =>
        setConfigError(err instanceof Error ? err.message : String(err)),
      );
    return unsub;
  }, [client]);

  async function toggle(channel: number, on: boolean): Promise<void> {
    setPending(prev => new Set(prev).add(channel));
    try {
      await client.setOutput(channel, on);
    } catch {
      // The status poll reflects true device state regardless of this failing.
    } finally {
      setPending(prev => {
        const next = new Set(prev);
        next.delete(channel);
        return next;
      });
    }
  }

  const lockLabel = status
    ? (MC_LOCK_STATE[status.lockState as 0 | 1 | 2 | 3] ?? 'UNKNOWN')
    : '—';
  const isLocked = status?.lockState === LOCK_STATE.LOCKED;
  const hasHazardGroup = !!config?.outputs.channels.some(c => c.hazard_member);
  const authed = client.isAuthenticated();

  async function hazardPress(): Promise<void> {
    setHazardBusy(true);
    setHazardError(null);
    try {
      const result = await client.hazardPress();
      if (!result.ok)
        setHazardError(`Hazard press rejected: ${result.resultName}`);
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
      if (!result.ok)
        setLockActionError(
          `${isLocked ? 'Unlock' : 'Lock'} rejected: ${result.resultName}`,
        );
    } catch (err) {
      setLockActionError(err instanceof Error ? err.message : String(err));
    } finally {
      setLockActionBusy(false);
    }
  }

  const showLockAction =
    status &&
    (status.lockState === LOCK_STATE.LOCKED ||
      status.lockState === LOCK_STATE.PARKED);

  return (
    <Screen
      title={deviceName}
      trailing={
        <Button label="Disconnect" tone="ghost" onPress={onDisconnect} />
      }
    >
      <View style={styles.statGrid}>
        <Stat
          label="Lock"
          value={lockLabel}
          tone={isLocked ? 'warn' : undefined}
        />
        <Stat
          label="Battery"
          value={status ? `${(status.batteryMv / 1000).toFixed(2)} V` : '—'}
          tone={status?.lvCutoffActive ? 'danger' : undefined}
        />
        <Stat
          label="Uptime"
          value={status ? formatUptime(status.uptimeMs) : '—'}
        />
        <Stat
          label="Firmware"
          value={
            status
              ? `${status.fwMajor}.${status.fwMinor}.${status.fwPatch}`
              : '—'
          }
        />
      </View>

      {!authed && (
        <Notice tone="warn">
          Not authenticated — control is unavailable until this phone's key is
          accepted.
        </Notice>
      )}
      {status?.lvCutoffActive && (
        <Notice tone="danger">
          Low-voltage cutoff active. Non-essential outputs are suppressed until
          the battery recovers.
        </Notice>
      )}
      {status && status.outputFaultMask !== 0 && (
        <Notice tone="warn">
          A channel has reported a fault. Open Diagnostics for detail.
        </Notice>
      )}
      {status?.cheatcodeBackoff && (
        <Notice tone="warn">
          Cheat-code entry is in backoff. Phone and ignition-switch unlock still
          work.
        </Notice>
      )}

      {(showLockAction || hasHazardGroup) && (
        <View style={styles.actionRow}>
          {showLockAction && (
            <Button
              style={styles.actionButton}
              label={
                lockActionBusy ? 'Working' : isLocked ? 'Unlock' : 'Lock now'
              }
              tone={isLocked ? 'primary' : 'secondary'}
              busy={lockActionBusy}
              disabled={!authed}
              onPress={quickLockToggle}
            />
          )}
          {hasHazardGroup && (
            <Button
              style={styles.actionButton}
              label={hazardBusy ? 'Working' : 'Hazards'}
              tone="danger"
              busy={hazardBusy}
              disabled={!authed}
              onPress={hazardPress}
            />
          )}
        </View>
      )}
      {lockActionError && <Notice tone="danger">{lockActionError}</Notice>}
      {hazardError && <Notice tone="danger">{hazardError}</Notice>}

      <SectionHeader>Setup</SectionHeader>
      <View style={styles.navGrid}>
        <NavTile
          label="Outputs"
          detail="Name channels, assign functions and modes"
          onPress={onOpenOutputs}
        />
        <NavTile
          label="Buttons"
          detail="Identify switches and bind them to outputs"
          onPress={onOpenButtons}
        />
        <NavTile
          label="Immobilizer"
          detail="Phone key, cheat-code, ignition switch"
          onPress={onOpenLock}
        />
        <NavTile
          label="Paired keys"
          detail="Enrolled phones, revoke, transfer"
          onPress={onOpenKeys}
        />
        <NavTile
          label="Diagnostics"
          detail="Per-channel current, faults, calibration"
          onPress={onOpenDiagnostics}
        />
        <NavTile
          label="Firmware"
          detail="Check for and install updates"
          onPress={onOpenFirmwareUpdate}
        />
        <NavTile
          label="Event log"
          detail="Lock, key and OTA history"
          onPress={onOpenEventLog}
        />
      </View>

      <SectionHeader>Outputs</SectionHeader>
      {configError && (
        <Notice tone="warn">{`Configuration unavailable: ${configError}`}</Notice>
      )}
      <Card padded={false}>
        {Array.from({ length: OUTPUT_COUNT }).map((_, ch) => {
          const chCfg = config?.outputs.channels[ch];
          const isStarter = chCfg?.is_starter === true;
          const on = status ? isOutputOn(status, ch) : false;
          const faulted = status
            ? (status.outputFaultMask & (1 << ch)) !== 0
            : false;
          return (
            <View key={ch}>
              {ch > 0 && <Divider />}
              <View style={styles.channelRow}>
                <View style={styles.channelPip}>
                  <Text style={styles.channelPipText}>{ch + 1}</Text>
                </View>
                <View style={styles.channelInfo}>
                  <Text style={styles.channelName} numberOfLines={1}>
                    {chCfg?.name?.trim() || `Output ${ch + 1}`}
                  </Text>
                  <View style={styles.channelMetaRow}>
                    <Text style={type.caption}>{chCfg?.behaviour ?? '—'}</Text>
                    {faulted && <Badge label="FAULT" tone="danger" />}
                    {isStarter && <Badge label="BUTTON ONLY" tone="neutral" />}
                  </View>
                </View>
                <Switch
                  value={on}
                  disabled={isStarter || pending.has(ch) || !authed}
                  onValueChange={v => toggle(ch, v)}
                  trackColor={{ false: colors.borderStrong, true: colors.on }}
                  thumbColor={colors.text}
                  ios_backgroundColor={colors.borderStrong}
                  accessibilityLabel={`${chCfg?.name?.trim() || `Output ${ch + 1}`}, ${on ? 'on' : 'off'}`}
                />
              </View>
            </View>
          );
        })}
      </Card>
    </Screen>
  );
}

/** Uptime as something a human reads at a glance, not raw seconds. */
function formatUptime(ms: number): string {
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  if (h > 0) return `${h}h ${m}m`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

const styles = StyleSheet.create({
  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },
  actionRow: { flexDirection: 'row', gap: space.sm },
  actionButton: { flex: 1 },

  navGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },
  navTile: {
    flexGrow: 1,
    flexBasis: '46%',
    minHeight: 76,
    backgroundColor: colors.raised,
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: radius.md,
    padding: space.md,
    gap: 2,
  },
  navTilePressed: {
    backgroundColor: colors.raisedHover,
    borderColor: colors.borderStrong,
  },
  navLabel: { ...type.body, fontWeight: '600' },

  channelRow: {
    flexDirection: 'row',
    alignItems: 'center',
    paddingVertical: space.md,
    paddingHorizontal: space.md,
    gap: space.md,
  },
  channelPip: {
    width: 28,
    height: 28,
    borderRadius: radius.sm,
    backgroundColor: colors.raisedHover,
    alignItems: 'center',
    justifyContent: 'center',
  },
  channelPipText: { ...type.valueSmall, fontWeight: '700' },
  channelInfo: { flex: 1, gap: 2 },
  channelName: { ...type.body, fontWeight: '600' },
  channelMetaRow: { flexDirection: 'row', alignItems: 'center', gap: space.sm },
});
