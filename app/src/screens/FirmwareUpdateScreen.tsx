/**
 * Firmware update (docs/PROTOCOL.md §10, §10.5). Checks the one
 * baked-in update-check URL (the only network call this app ever makes
 * outside BLE), downloads the signed `.mcota` bundle if the rider chooses to
 * update, and drives the OTA transfer over BLE via MotoClient.
 *
 * Reboot is a separate, explicit step after a successful transfer (mirrors
 * mc_ota's own COMMITTED state, docs/PROTOCOL.md §10.1) — a rider can
 * finish a ride first, then come back and apply it.
 *
 * A failed manifest/bundle fetch is shown as "unable to check for
 * updates," never as a screen-blocking error — this network path is
 * optional and must never degrade BLE control of the board.
 */
import React, { useCallback, useEffect, useState } from 'react';
import {
  ActivityIndicator,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from 'react-native';

import { OTA_STATE } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import type {
  FirmwareBundle,
  OtaStatus,
  UpdateManifest,
} from '../protocol/types';
import {
  downloadFirmwareBundle,
  fetchUpdateManifest,
  isNewerVersion,
} from '../update/updateCheck';
import { Screen } from '../ui/components';
import { colors } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

type Phase =
  | 'idle'
  | 'checking'
  | 'up-to-date'
  | 'available'
  | 'downloading'
  | 'uploading'
  | 'ready'
  | 'error';

function currentVersionString(client: MotoClient): string {
  const status = client.getLastStatus();
  return status ? `${status.fwMajor}.${status.fwMinor}.${status.fwPatch}` : '?';
}

export function FirmwareUpdateScreen({
  client,
  onDone,
}: Props): React.JSX.Element {
  const [phase, setPhase] = useState<Phase>('idle');
  const [manifest, setManifest] = useState<UpdateManifest | null>(null);
  const [bundle, setBundle] = useState<FirmwareBundle | null>(null);
  const [progress, setProgress] = useState<{
    sent: number;
    total: number;
  } | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [deviceOta, setDeviceOta] = useState<OtaStatus | null>(null);
  const [rebootBusy, setRebootBusy] = useState(false);

  const currentVersion = currentVersionString(client);

  const refreshDeviceOta = useCallback(() => {
    client
      .otaStatus()
      .then(s => setDeviceOta(s))
      .catch(() => {});
  }, [client]);

  useEffect(() => {
    refreshDeviceOta();
  }, [refreshDeviceOta]);

  async function checkForUpdate(): Promise<void> {
    setPhase('checking');
    setError(null);
    try {
      const m = await fetchUpdateManifest();
      setManifest(m);
      setPhase(
        isNewerVersion(currentVersion, m.version) ? 'available' : 'up-to-date',
      );
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
      setPhase('error');
    }
  }

  async function downloadAndInstall(): Promise<void> {
    if (!manifest) return;
    setError(null);
    setPhase('downloading');
    try {
      const b = await downloadFirmwareBundle(manifest);
      setBundle(b);
      setPhase('uploading');
      setProgress({ sent: 0, total: b.image.length });
      const result = await client.uploadFirmware(b, (sent, total) =>
        setProgress({ sent, total }),
      );
      if (!result.ok) {
        setError(`Device rejected the update: ${result.resultName}`);
        setPhase('error');
        return;
      }
      setPhase('ready');
      refreshDeviceOta();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
      setPhase('error');
    }
  }

  async function applyNow(): Promise<void> {
    setRebootBusy(true);
    setError(null);
    try {
      const result = await client.otaReboot();
      if (!result.ok) {
        setError(
          `Reboot refused: ${result.resultName} — the bike may be riding or the battery too low; try again once stopped.`,
        );
      }
      refreshDeviceOta();
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setRebootBusy(false);
    }
  }

  async function abort(): Promise<void> {
    await client.otaAbort().catch(() => {});
    setPhase('idle');
    setBundle(null);
    setProgress(null);
    refreshDeviceOta();
  }

  return (
    <Screen title="Firmware Update" onBack={onDone}>
      <Text style={styles.hint}>Current device firmware: {currentVersion}</Text>

      {deviceOta &&
        deviceOta.state === OTA_STATE.COMMITTED &&
        phase === 'idle' && (
          <View style={styles.banner}>
            <Text style={styles.bannerText}>
              A previously downloaded update is already staged on the device (
              {deviceOta.bytesReceived} bytes) and ready to apply.
            </Text>
            <TouchableOpacity
              style={styles.button}
              onPress={applyNow}
              disabled={rebootBusy}
            >
              <Text style={styles.buttonText}>
                {rebootBusy ? 'Working…' : 'Apply staged update now'}
              </Text>
            </TouchableOpacity>
          </View>
        )}

      {(phase === 'idle' || phase === 'error' || phase === 'up-to-date') && (
        <TouchableOpacity style={styles.button} onPress={checkForUpdate}>
          <Text style={styles.buttonText}>Check for updates</Text>
        </TouchableOpacity>
      )}
      {phase === 'checking' && (
        <View style={styles.row}>
          <ActivityIndicator />
          <Text style={styles.hint}>Checking…</Text>
        </View>
      )}
      {phase === 'up-to-date' && (
        <Text style={styles.hint}>You're up to date.</Text>
      )}
      {phase === 'error' && error && (
        <Text style={styles.error}>Unable to check for updates: {error}</Text>
      )}

      {phase === 'available' && manifest && (
        <View style={styles.banner}>
          <Text style={styles.bannerTitle}>
            Update available: {manifest.version}
          </Text>
          {manifest.changelog ? (
            <Text style={styles.hint}>{manifest.changelog}</Text>
          ) : null}
          <TouchableOpacity style={styles.button} onPress={downloadAndInstall}>
            <Text style={styles.buttonText}>
              Download &amp; transfer to device
            </Text>
          </TouchableOpacity>
        </View>
      )}

      {phase === 'downloading' && (
        <View style={styles.row}>
          <ActivityIndicator />
          <Text style={styles.hint}>Downloading update…</Text>
        </View>
      )}

      {phase === 'uploading' && progress && (
        <View>
          <Text style={styles.hint}>
            Transferring over Bluetooth: {progress.sent} / {progress.total}{' '}
            bytes (
            {Math.round((progress.sent / Math.max(1, progress.total)) * 100)}%)
          </Text>
          <TouchableOpacity
            style={[styles.button, styles.dangerButton]}
            onPress={abort}
          >
            <Text style={styles.buttonText}>Cancel</Text>
          </TouchableOpacity>
        </View>
      )}

      {phase === 'ready' && bundle && (
        <View style={styles.banner}>
          <Text style={styles.bannerTitle}>
            Transfer complete — ready to apply.
          </Text>
          <Text style={styles.hint}>
            The current firmware keeps running until you apply the update. You
            can do this now or wait until you're done riding.
          </Text>
          <TouchableOpacity
            style={styles.button}
            onPress={applyNow}
            disabled={rebootBusy}
          >
            <Text style={styles.buttonText}>
              {rebootBusy ? 'Working…' : 'Apply now (device will reboot)'}
            </Text>
          </TouchableOpacity>
          <TouchableOpacity
            style={[styles.button, styles.dangerButton]}
            onPress={abort}
          >
            <Text style={styles.buttonText}>Discard staged update</Text>
          </TouchableOpacity>
        </View>
      )}
      {error && phase !== 'error' && <Text style={styles.error}>{error}</Text>}
    </Screen>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: colors.bg },
  content: { padding: 16, gap: 10 },
  header: {
    flexDirection: 'row',
    justifyContent: 'space-between',
    alignItems: 'center',
  },
  title: { color: colors.text, fontSize: 20, fontWeight: '700' },
  link: { color: colors.accent },
  hint: { fontSize: 12, color: colors.textMuted },
  row: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  button: {
    padding: 12,
    borderRadius: 8,
    alignItems: 'center',
    backgroundColor: colors.accent,
    marginTop: 8,
  },
  dangerButton: { backgroundColor: colors.danger },
  buttonText: { color: colors.textOnAccent, fontWeight: '600' },
  banner: {
    backgroundColor: colors.raised,
    borderRadius: 8,
    padding: 12,
    gap: 4,
  },
  bannerTitle: { color: colors.text, fontWeight: '700' },
  bannerText: { color: colors.text, fontSize: 13 },
  error: { color: colors.danger },
});
