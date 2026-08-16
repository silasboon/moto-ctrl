/**
 * MOTO-CTRL companion app.
 *
 * Navigation is local component state, not a router — the shape here is two
 * levels of drill-down, which react-navigation and its native peers would be
 * a lot of dependency to express (see app/NATIVE_SETUP.md).
 *
 * Ride is the landing screen and the only thing shown on launch/reconnect —
 * it's the only screen anyone opens with the engine warm, so it gets the
 * whole screen with no menu competing for it. Everything else (board name,
 * output channels, button bindings, paired phones, immobilizer, diagnostics,
 * firmware, event log) lives one tap away behind the round settings button
 * in Ride's title bar, on the Settings screen — grouped into named sections
 * (Configuration / Device) rather than flattened into one undifferentiated
 * list, since those groupings are still meaningful even without being
 * separate tabs. Each section row pushes one detail screen deep, same as
 * before.
 *
 * A rider can leave a half-edited config by swiping/tapping back rather than
 * using the screen's own Back chevron, so every way out the shell itself
 * drives (edge-swipe) is routed through NavGuard — the mounted screen's
 * `useLeaveGuard` gets to confirm first, exactly as its own chevron does.
 */
import React, { useCallback, useEffect, useState } from 'react';
import { StatusBar, StyleSheet, View } from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';

import { colors } from './src/ui/theme';
import { EdgeSwipeBack } from './src/ui/EdgeSwipeBack';
import { NavGuardProvider, useNavGuard } from './src/ui/NavGuard';

import { BoardInfoCard } from './src/screens/BoardInfoCard';
import { BoardScreen } from './src/screens/BoardScreen';
import { ButtonsScreen } from './src/screens/ButtonsScreen';
import { DashboardScreen } from './src/screens/DashboardScreen';
import { DiagnosticsScreen } from './src/screens/DiagnosticsScreen';
import { EventLogScreen } from './src/screens/EventLogScreen';
import { FirmwareUpdateScreen } from './src/screens/FirmwareUpdateScreen';
import { KeysScreen } from './src/screens/KeysScreen';
import { LockScreen } from './src/screens/LockScreen';
import { PairingScreen } from './src/screens/PairingScreen';
import { OutputsScreen } from './src/screens/OutputsScreen';
import { SettingsScreen } from './src/screens/SettingsScreen';
import type { MotoClient } from './src/protocol/MotoClient';
import type { DeviceDescriptor } from './src/transport/Transport';

type Detail =
  | 'board'
  | 'outputs'
  | 'buttons'
  | 'keys'
  | 'lock'
  | 'diagnostics'
  | 'firmwareUpdate'
  | 'eventLog';

interface Session {
  client: MotoClient;
  device: DeviceDescriptor;
}

function AppContent(): React.JSX.Element {
  const nav = useNavGuard();
  const [session, setSession] = useState<Session | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [detail, setDetail] = useState<Detail | null>(null);
  /* Surfaced on the pairing screen after an unexpected drop, so a board going
   * out of range reads as a plain message rather than whatever error the
   * in-flight request happened to reject with. */
  const [notice, setNotice] = useState<string | null>(null);

  const handlePaired = useCallback(
    (client: MotoClient, device: DeviceDescriptor) => {
      setNotice(null);
      setSession({ client, device });
      setSettingsOpen(false);
      setDetail(null);
    },
    [],
  );

  const handleDisconnect = useCallback(() => {
    if (!session) return;
    setNotice(null);
    session.client.disconnect().finally(() => setSession(null));
  }, [session]);

  /* An unexpected drop — out of range, board powered down, BLE turned off.
   * Tear the session down and go back to pairing, which immediately starts
   * scanning and reconnects on its own once the board is back. */
  useEffect(() => {
    if (!session) return undefined;
    const unsub = session.client.onConnectionStateChange(state => {
      if (state === 'disconnected') {
        setNotice(
          `Lost connection to ${session.device.name}. Searching for it again…`,
        );
        setSession(null);
        setSettingsOpen(false);
        setDetail(null);
      }
    });
    return unsub;
  }, [session]);

  // Ownership transfer wipes every enrolled key (including this phone's) and
  // resets lock config device-side — treat it like a disconnect back to
  // Pairing. If this is the same phone, PairingScreen's existing
  // authenticate-then-fall-back-to-enroll flow re-enrolls it via
  // trust-on-first-use, since the keystore is now empty.
  const handleOwnershipTransferred = useCallback(() => {
    handleDisconnect();
  }, [handleDisconnect]);

  if (!session) {
    return (
      <SafeAreaView style={styles.container}>
        <StatusBar barStyle="light-content" backgroundColor={colors.bg} />
        <PairingScreen onPaired={handlePaired} notice={notice} />
      </SafeAreaView>
    );
  }

  const client = session.client;
  const closeDetail = (): void => setDetail(null);
  const closeSettings = (): void => setSettingsOpen(false);

  /* Every way OUT of a detail screen that the shell owns has to go through
   * the mounted screen's guard, or the confirmation is only as good as the
   * route the rider happened to take. The screen's own Back chevron is
   * already guarded inside useLeaveGuard; this is the one the shell drives
   * — an edge swipe. (Settings itself carries no dirty state of its own, so
   * closing it doesn't need guarding — same as a tab switch never did.) */
  const guarded = (proceed: () => void): void => {
    if (nav) nav.run(proceed);
    else proceed();
  };

  const detailScreen = ((): React.JSX.Element | null => {
    switch (detail) {
      case 'board':
        return <BoardScreen client={client} onDone={closeDetail} />;
      case 'outputs':
        return <OutputsScreen client={client} onDone={closeDetail} />;
      case 'buttons':
        return <ButtonsScreen client={client} onDone={closeDetail} />;
      case 'keys':
        return <KeysScreen client={client} onDone={closeDetail} />;
      case 'lock':
        return (
          <LockScreen
            client={client}
            onDone={closeDetail}
            onOwnershipTransferred={handleOwnershipTransferred}
          />
        );
      case 'diagnostics':
        return <DiagnosticsScreen client={client} onDone={closeDetail} />;
      case 'firmwareUpdate':
        return <FirmwareUpdateScreen client={client} onDone={closeDetail} />;
      case 'eventLog':
        return <EventLogScreen client={client} onDone={closeDetail} />;
      default:
        return null;
    }
  })();

  const settingsScreen = (
    <SettingsScreen
      header={<BoardInfoCard client={client} />}
      configurationItems={[
        {
          label: 'Outputs',
          detail: 'Name channels, choose what each one does',
          onPress: () => setDetail('outputs'),
        },
        {
          label: 'Buttons',
          detail: 'Identify switches and bind them to outputs',
          onPress: () => setDetail('buttons'),
        },
        {
          label: 'Immobilizer',
          detail: 'Phone key, cheat-code, ignition switch',
          onPress: () => setDetail('lock'),
        },
        {
          label: 'Paired keys',
          detail: 'Enrolled phones, revoke, ownership transfer',
          onPress: () => setDetail('keys'),
        },
      ]}
      deviceItems={[
        {
          label: 'Board',
          detail: 'Name this board',
          onPress: () => setDetail('board'),
        },
        {
          label: 'Diagnostics',
          detail: 'Per-channel current, faults, calibration',
          onPress: () => setDetail('diagnostics'),
        },
        {
          label: 'Firmware',
          detail: 'Check for and install updates',
          onPress: () => setDetail('firmwareUpdate'),
        },
        {
          label: 'Event log',
          detail: 'Lock, key and OTA history',
          onPress: () => setDetail('eventLog'),
        },
      ]}
      onBack={closeSettings}
      onDisconnect={handleDisconnect}
    />
  );

  return (
    <SafeAreaView style={styles.container}>
      <StatusBar barStyle="light-content" backgroundColor={colors.bg} />
      <View style={styles.body}>
        {detail ? (
          <EdgeSwipeBack onBack={() => guarded(closeDetail)}>
            {detailScreen}
          </EdgeSwipeBack>
        ) : settingsOpen ? (
          <EdgeSwipeBack onBack={closeSettings}>{settingsScreen}</EdgeSwipeBack>
        ) : (
          <DashboardScreen
            client={client}
            deviceName={session.device.name}
            onOpenSettings={() => setSettingsOpen(true)}
          />
        )}
      </View>
    </SafeAreaView>
  );
}

/**
 * Safe-area insets come from react-native-safe-area-context, not RN core.
 * Core's SafeAreaView is deprecated (and always was iOS-only — it did nothing
 * on Android, which now matters: RN 0.86 targets Android 15's enforced
 * edge-to-edge, so content would otherwise sit under the status and gesture
 * bars). The provider must wrap everything that reads an inset, so it lives
 * here rather than inside any one screen.
 */
function App(): React.JSX.Element {
  return (
    <SafeAreaProvider>
      <NavGuardProvider>
        <AppContent />
      </NavGuardProvider>
    </SafeAreaProvider>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    /* Set here as well as on each Screen, so the safe-area insets above and
     * below the content match the app background instead of flashing white. */
    backgroundColor: colors.bg,
  },
  body: { flex: 1 },
});

export default App;
