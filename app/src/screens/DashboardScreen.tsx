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
import { StyleSheet, Text, View } from 'react-native';

import { LOCK_METHOD, LOCK_STATE, MC_LOCK_STATE } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import {
  isOutputOn,
  type DeviceConfig,
  type LockConfig,
  type Status,
} from '../protocol/types';
import {
  Button,
  Divider,
  Notice,
  RoundIconButton,
  Screen,
} from '../ui/components';
import { colors, radius, space, type } from '../ui/theme';

interface Props {
  client: MotoClient;
  deviceName: string;
  onOpenSettings: () => void;
}

export function DashboardScreen({
  client,
  deviceName,
  onOpenSettings,
}: Props): React.JSX.Element {
  const [status, setStatus] = useState<Status | null>(client.getLastStatus());
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  /* Whether an immobilizer exists at all can't be read off the status wire:
   * mc_lock_wire_state() reports DISABLED as UNLOCKED, so "no immobilizer
   * configured" and "unlocked" are the same byte. The lock config is the only
   * place that distinguishes them. */
  const [lockConfig, setLockConfig] = useState<LockConfig | null>(null);
  const [lockActionBusy, setLockActionBusy] = useState(false);
  const [lockActionError, setLockActionError] = useState<string | null>(null);
  const [hazardBusy, setHazardBusy] = useState(false);
  const [hazardError, setHazardError] = useState<string | null>(null);

  useEffect(() => {
    const unsub = client.onStatus(setStatus);
    /* Only the board name and the hazard-group check need this now that the
     * channel rows have moved to Settings → Outputs. */
    client
      .configRead()
      .then(setConfig)
      .catch(() => {});
    /* Best-effort: a board that can't answer simply shows no lock card
     * rather than blocking the screen a rider opened to see live state. */
    client
      .lockGetConfig()
      .then(setLockConfig)
      .catch(() => {});
    return unsub;
  }, [client]);

  const lockLabel = status
    ? (MC_LOCK_STATE[status.lockState as 0 | 1 | 2 | 3] ?? 'UNKNOWN')
    : '—';
  const isLocked = status?.lockState === LOCK_STATE.LOCKED;
  const hasHazardGroup = !!config?.outputs.channels.some(c => c.hazard_member);
  /* Comes from its own status bit, not from outputStateMask: hazard members
   * blink, so the mask alternates several times a second and can't answer
   * "are the hazards on". Without this the button was unlabelled state — a
   * rider pressing it had no way to tell whether they were starting or
   * stopping them. */
  const hazardsOn = !!status?.hazardActive;
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
      if (isLocked) {
        const result = await client.unlock();
        if (!result.ok)
          setLockActionError(`Unlock rejected: ${result.resultName}`);
        return;
      }

      /* Turn the key off before locking. mc_lock_request_lock() refuses
       * while the ignition output is live (AGENTS.md #2), and unlocking
       * deliberately switched it on — so without this, a bike unlocked from
       * the app could never be locked again from the app. Doing it here
       * rather than relaxing the firmware guard keeps the invariant exactly
       * as written: at the moment LOCKED is entered, the ignition is off. */
      if (
        ignitionChannel >= 0 &&
        status &&
        isOutputOn(status, ignitionChannel)
      ) {
        await client.setOutput(ignitionChannel, false);
      }
      const result = await client.lock();
      if (!result.ok)
        setLockActionError(
          result.resultName === 'REJECTED'
            ? "Can't lock while the engine is running. Switch it off and try again."
            : `Lock rejected: ${result.resultName}`,
        );
    } catch (err) {
      setLockActionError(err instanceof Error ? err.message : String(err));
    } finally {
      setLockActionBusy(false);
    }
  }

  /* The immobilizer is the thing a rider comes to this screen for, so the
   * control is always present once one is configured — not only in the two
   * states it used to appear in, which meant walking up to a bike and finding
   * no lock button because it hadn't finished parking yet.
   *
   * DISABLED is the one state with nothing to offer: no immobilizer is
   * configured, so there is nothing to lock. */
  const hasImmobilizer = lockConfig?.immobilizerEnabled === true;
  /* Unlocking from the app only works when phone-as-key is a configured
   * method — mc_lock_request_unlock() returns UNAUTHORIZED otherwise. With it
   * off, this bike is deliberately one you get into with the cheat-code or
   * the ignition switch, so an Unlock button would be a control that cannot
   * work.
   *
   * Locking is NOT gated the same way (mc_lock_request_lock has no method
   * check): securing a bike you are already authenticated to is never the
   * risky direction, so that button stays regardless. */
  const phoneKeyEnabled =
    lockConfig !== null && (lockConfig.methodsMask & LOCK_METHOD.PHONE) !== 0;
  const canUnlockFromApp = hasImmobilizer && phoneKeyEnabled;

  /* What to tell a rider who has no app unlock button. */
  const otherWayIn = ((): string => {
    const ways: string[] = [];
    if (lockConfig?.cheatcodeSet) {
      ways.push('enter your cheat-code on the handlebar buttons');
    }
    if (
      lockConfig !== null &&
      (lockConfig.methodsMask & LOCK_METHOD.IGNITION_SWITCH) !== 0 &&
      lockConfig.ignitionSwitchInput >= 0
    ) {
      ways.push('turn the ignition switch');
    }
    return ways.length === 0
      ? 'Phone unlock is off and no other method is configured — check Settings.'
      : `Phone unlock is off for this bike: ${ways.join(', or ')}.`;
  })();
  /* Locking is refused by the firmware while the engine runs or the ignition
   * is live (AGENTS.md #2) — say so in advance rather than offering a button
   * that returns REJECTED. */
  /* The ignition channel, so locking can switch it off first — see
   * quickLockToggle(). -1 when the config hasn't arrived or none is set. */
  const ignitionChannel =
    config?.outputs.channels.findIndex(c => c.is_ignition) ?? -1;

  return (
    <Screen
      /* Prefer the name the board itself reports over the one the scan
       * saw: a rename lands in the config immediately, while the advertised
       * name a phone has cached can lag until the next discovery. */
      title={config?.device_name?.trim() || deviceName}
      trailing={
        <RoundIconButton
          glyph="⚙"
          accessibilityLabel="Settings"
          onPress={onOpenSettings}
        />
      }
      scroll={false}
    >
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
      {lockActionError && <Notice tone="danger">{lockActionError}</Notice>}
      {hazardError && <Notice tone="danger">{hazardError}</Notice>}

      {/* The lock state takes the room, because it is the one thing worth
       * reading from arm's length while pulling gloves on.
       *
       * lockConfig arrives async (lockGetConfig(), useEffect above); until it
       * does, hasImmobilizer reads false the same as "genuinely disabled"
       * would, which used to flash "NOT SET UP" for a beat on every screen
       * open before flipping to the real state. Gating on lockConfig === null
       * distinguishes "don't know yet" from "know, and it's off". */}
      <View style={styles.hero}>
        <Text style={styles.heroLabel}>
          {lockConfig === null
            ? ' '
            : hasImmobilizer
              ? 'Immobilizer'
              : 'Immobilizer off'}
        </Text>
        <Text
          style={[styles.heroState, isLocked && styles.heroStateLocked]}
          numberOfLines={1}
          adjustsFontSizeToFit
        >
          {lockConfig === null ? '—' : hasImmobilizer ? lockLabel : 'NOT SET UP'}
        </Text>
        <Text style={styles.heroDetail}>
          {lockConfig === null
            ? ' '
            : !hasImmobilizer
              ? 'Set one up under Settings to lock this bike from your phone.'
              : isLocked
                ? canUnlockFromApp
                  ? 'Nothing switches on until you unlock. Unlocking turns the ignition on, ready to start.'
                  : otherWayIn
                : 'Locking switches the ignition off, cuts every output, and blocks the starter.'}
        </Text>
      </View>

      {hasImmobilizer && (!isLocked || canUnlockFromApp) && (
        <Button
          size="large"
          label={lockActionBusy ? 'Working' : isLocked ? 'UNLOCK' : 'LOCK'}
          tone={isLocked ? 'primary' : 'secondary'}
          busy={lockActionBusy}
          disabled={!authed}
          onPress={quickLockToggle}
        />
      )}

      {hasHazardGroup && (
        <Button
          size="large"
          label={hazardBusy ? 'Working' : hazardsOn ? 'HAZARDS OFF' : 'HAZARDS'}
          /* Filled while running, outlined while not, so the state reads at a
           * glance and not only from the text. Never disabled by the lock:
           * hazards are the one control a locked bike keeps. */
          tone={hazardsOn ? 'danger' : 'secondary'}
          busy={hazardBusy}
          disabled={!authed}
          onPress={hazardPress}
        />
      )}

      <Divider />
      <View style={styles.batteryRow}>
        <Text style={styles.batteryLabel}>Battery</Text>
        <Text
          style={[
            styles.batteryValue,
            status?.lvCutoffActive && styles.batteryLow,
          ]}
        >
          {status ? `${(status.batteryMv / 1000).toFixed(2)}` : '—'}
          <Text style={styles.batteryUnit}> V</Text>
        </Text>
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  /* Takes whatever vertical room is going, so the lock state is the thing
   * the eye lands on. */
  hero: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
    gap: space.sm,
    paddingHorizontal: space.md,
  },
  heroLabel: { ...type.overline },
  heroState: {
    fontSize: 56,
    lineHeight: 64,
    fontWeight: '800',
    letterSpacing: -1.5,
    color: colors.on,
  },
  heroStateLocked: { color: colors.warn },
  heroDetail: { ...type.caption, textAlign: 'center', maxWidth: 320 },

  batteryRow: {
    flexDirection: 'row',
    alignItems: 'baseline',
    justifyContent: 'space-between',
    paddingHorizontal: space.md,
    paddingTop: space.md,
  },
  batteryLabel: { ...type.overline },
  batteryValue: { ...type.value, fontSize: 30, fontWeight: '700' },
  batteryUnit: { fontSize: 16, color: colors.textMuted },
  batteryLow: { color: colors.danger },

  statGrid: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },
  actionRow: { flexDirection: 'row', gap: space.sm },
  actionButton: { flex: 1 },

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
