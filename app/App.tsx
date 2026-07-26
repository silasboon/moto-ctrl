/**
 * MOTO-CTRL companion app.
 *
 * Screen switching is local component state, not a router — four screens
 * doesn't justify the native dependencies (react-navigation + its native
 * peers) that come with one, and this environment has no generated native
 * project to verify they'd link (see app/NATIVE_SETUP.md).
 */
import React, { useCallback, useState } from 'react';
import { SafeAreaView, StyleSheet } from 'react-native';

import { DashboardScreen } from './src/screens/DashboardScreen';
import { DiagnosticsScreen } from './src/screens/DiagnosticsScreen';
import { EventLogScreen } from './src/screens/EventLogScreen';
import { FirmwareUpdateScreen } from './src/screens/FirmwareUpdateScreen';
import { KeysScreen } from './src/screens/KeysScreen';
import { LockScreen } from './src/screens/LockScreen';
import { PairingScreen } from './src/screens/PairingScreen';
import { PinMapperScreen } from './src/screens/PinMapperScreen';
import type { MotoClient } from './src/protocol/MotoClient';
import type { DeviceDescriptor } from './src/transport/Transport';

type Screen = 'dashboard' | 'pinMapper' | 'keys' | 'lock' | 'diagnostics' | 'firmwareUpdate' | 'eventLog';

interface Session {
  client: MotoClient;
  device: DeviceDescriptor;
}

function App(): React.JSX.Element {
  const [session, setSession] = useState<Session | null>(null);
  const [screen, setScreen] = useState<Screen>('dashboard');

  const handlePaired = useCallback((client: MotoClient, device: DeviceDescriptor) => {
    setSession({ client, device });
    setScreen('dashboard');
  }, []);

  const handleDisconnect = useCallback(() => {
    if (!session) return;
    session.client.disconnect().finally(() => setSession(null));
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
        <PairingScreen onPaired={handlePaired} />
      </SafeAreaView>
    );
  }

  return (
    <SafeAreaView style={styles.container}>
      {screen === 'dashboard' && (
        <DashboardScreen
          client={session.client}
          deviceName={session.device.name}
          onOpenPinMapper={() => setScreen('pinMapper')}
          onOpenKeys={() => setScreen('keys')}
          onOpenLock={() => setScreen('lock')}
          onOpenDiagnostics={() => setScreen('diagnostics')}
          onOpenFirmwareUpdate={() => setScreen('firmwareUpdate')}
          onOpenEventLog={() => setScreen('eventLog')}
          onDisconnect={handleDisconnect}
        />
      )}
      {screen === 'pinMapper' && <PinMapperScreen client={session.client} onDone={() => setScreen('dashboard')} />}
      {screen === 'keys' && <KeysScreen client={session.client} onDone={() => setScreen('dashboard')} />}
      {screen === 'lock' && (
        <LockScreen
          client={session.client}
          onDone={() => setScreen('dashboard')}
          onOwnershipTransferred={handleOwnershipTransferred}
        />
      )}
      {screen === 'diagnostics' && <DiagnosticsScreen client={session.client} onDone={() => setScreen('dashboard')} />}
      {screen === 'firmwareUpdate' && <FirmwareUpdateScreen client={session.client} onDone={() => setScreen('dashboard')} />}
      {screen === 'eventLog' && <EventLogScreen client={session.client} onDone={() => setScreen('dashboard')} />}
    </SafeAreaView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
  },
});

export default App;
