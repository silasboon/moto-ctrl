/**
 * MOTO-CTRL companion app.
 *
 * Screen switching is local component state, not a router — four screens
 * doesn't justify the native dependencies (react-navigation + its native
 * peers) that come with one, and this environment has no generated native
 * project to verify they'd link (see app/NATIVE_SETUP.md).
 */
import React, { useCallback, useEffect, useState } from 'react';
import { SafeAreaView, StatusBar, StyleSheet } from 'react-native';

import { colors } from './src/ui/theme';

import { ButtonsScreen } from './src/screens/ButtonsScreen';
import { DashboardScreen } from './src/screens/DashboardScreen';
import { DiagnosticsScreen } from './src/screens/DiagnosticsScreen';
import { EventLogScreen } from './src/screens/EventLogScreen';
import { FirmwareUpdateScreen } from './src/screens/FirmwareUpdateScreen';
import { KeysScreen } from './src/screens/KeysScreen';
import { LockScreen } from './src/screens/LockScreen';
import { PairingScreen } from './src/screens/PairingScreen';
import { OutputsScreen } from './src/screens/OutputsScreen';
import type { MotoClient } from './src/protocol/MotoClient';
import type { DeviceDescriptor } from './src/transport/Transport';

type Screen =
  | 'dashboard'
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

function App(): React.JSX.Element {
  const [session, setSession] = useState<Session | null>(null);
  const [screen, setScreen] = useState<Screen>('dashboard');
  /* Surfaced on the pairing screen after an unexpected drop, so a board going
   * out of range reads as a plain message rather than whatever error the
   * in-flight request happened to reject with. */
  const [notice, setNotice] = useState<string | null>(null);

  const handlePaired = useCallback(
    (client: MotoClient, device: DeviceDescriptor) => {
      setNotice(null);
      setSession({ client, device });
      setScreen('dashboard');
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
        setScreen('dashboard');
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

  return (
    <SafeAreaView style={styles.container}>
      <StatusBar barStyle="light-content" backgroundColor={colors.bg} />
      {screen === 'dashboard' && (
        <DashboardScreen
          client={session.client}
          deviceName={session.device.name}
          onOpenOutputs={() => setScreen('outputs')}
          onOpenButtons={() => setScreen('buttons')}
          onOpenKeys={() => setScreen('keys')}
          onOpenLock={() => setScreen('lock')}
          onOpenDiagnostics={() => setScreen('diagnostics')}
          onOpenFirmwareUpdate={() => setScreen('firmwareUpdate')}
          onOpenEventLog={() => setScreen('eventLog')}
          onDisconnect={handleDisconnect}
        />
      )}
      {screen === 'outputs' && (
        <OutputsScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
      {screen === 'buttons' && (
        <ButtonsScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
      {screen === 'keys' && (
        <KeysScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
      {screen === 'lock' && (
        <LockScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
          onOwnershipTransferred={handleOwnershipTransferred}
        />
      )}
      {screen === 'diagnostics' && (
        <DiagnosticsScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
      {screen === 'firmwareUpdate' && (
        <FirmwareUpdateScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
      {screen === 'eventLog' && (
        <EventLogScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
        />
      )}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    /* Set here as well as on each Screen, so the safe-area insets above and
     * below the content match the app background instead of flashing white. */
    backgroundColor: colors.bg,
  },
});

export default App;
