/**
 * MOTO-CTRL companion app.
 *
 * Navigation is local component state, not a router — the shape here is a
 * bottom tab bar over a single level of drill-down, which react-navigation
 * and its native peers would be a lot of dependency to express (see
 * app/NATIVE_SETUP.md).
 *
 * Four tabs, grouped by what a rider is doing rather than by screen:
 *
 *   Ride      live control — the only tab anyone opens with the engine warm
 *   Setup     board name, output channels, button bindings
 *   Security  paired phones, immobilizer
 *   Settings  board info, diagnostics, firmware, event log
 *
 * The Ride tab is the landing screen and holds no menu. The other three are
 * lists that push one detail screen deep.
 *
 * The tab bar stays visible at every level, including on detail screens. That
 * means a rider can leave a half-edited config by tapping a tab rather than
 * Back, so tab switches are routed through NavGuard — the mounted screen's
 * `useLeaveGuard` gets to confirm first, exactly as it does for Back. The bar
 * also overlays the content rather than sitting above it in the layout, which
 * is what gives the Liquid Glass material something to refract; screens are
 * inset by `useTabBarHeight()` to compensate.
 */
import React, { useCallback, useEffect, useState } from 'react';
import { StatusBar, StyleSheet, View } from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';

import { colors } from './src/ui/theme';
import { BottomInsetProvider } from './src/ui/components';
import { EdgeSwipeBack } from './src/ui/EdgeSwipeBack';
import { NavGuardProvider, useNavGuard } from './src/ui/NavGuard';
import { TabBar, useTabBarHeight, type TabDef } from './src/ui/TabBar';

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
import { SectionScreen } from './src/screens/SectionScreen';
import type { MotoClient } from './src/protocol/MotoClient';
import type { DeviceDescriptor } from './src/transport/Transport';

type Tab = 'ride' | 'setup' | 'security' | 'settings';

type Detail =
  | 'board'
  | 'outputs'
  | 'buttons'
  | 'keys'
  | 'lock'
  | 'diagnostics'
  | 'firmwareUpdate'
  | 'eventLog';

const TABS: readonly TabDef<Tab>[] = [
  { key: 'ride', label: 'Ride' },
  { key: 'setup', label: 'Setup' },
  { key: 'security', label: 'Security' },
  { key: 'settings', label: 'Settings' },
];

interface Session {
  client: MotoClient;
  device: DeviceDescriptor;
}

function AppContent(): React.JSX.Element {
  const nav = useNavGuard();
  const tabBarHeight = useTabBarHeight();
  const [session, setSession] = useState<Session | null>(null);
  const [tab, setTab] = useState<Tab>('ride');
  const [detail, setDetail] = useState<Detail | null>(null);
  /* Surfaced on the pairing screen after an unexpected drop, so a board going
   * out of range reads as a plain message rather than whatever error the
   * in-flight request happened to reject with. */
  const [notice, setNotice] = useState<string | null>(null);

  const handlePaired = useCallback(
    (client: MotoClient, device: DeviceDescriptor) => {
      setNotice(null);
      setSession({ client, device });
      setTab('ride');
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
        setTab('ride');
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

  /* Every way OUT of a detail screen that the shell owns has to go through
   * the mounted screen's guard, or the confirmation is only as good as the
   * route the rider happened to take. The screen's own Back chevron is
   * already guarded inside useLeaveGuard; these are the two the shell drives
   * — a tab tap and an edge swipe. */
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

  const tabScreen = ((): React.JSX.Element => {
    switch (tab) {
      case 'setup':
        return (
          <SectionScreen
            title="Setup"
            items={[
              {
                label: 'Board',
                detail: 'Name this board',
                onPress: () => setDetail('board'),
              },
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
            ]}
          />
        );
      case 'security':
        return (
          <SectionScreen
            title="Security"
            items={[
              {
                label: 'Paired keys',
                detail: 'Enrolled phones, revoke, ownership transfer',
                onPress: () => setDetail('keys'),
              },
              {
                label: 'Immobilizer',
                detail: 'Phone key, cheat-code, ignition switch',
                onPress: () => setDetail('lock'),
              },
            ]}
          />
        );
      case 'settings':
        return (
          <SectionScreen
            title="Settings"
            header={<BoardInfoCard client={client} />}
            items={[
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
          />
        );
      default:
        return (
          <DashboardScreen
            client={client}
            deviceName={session.device.name}
            onDisconnect={handleDisconnect}
          />
        );
    }
  })();

  /* The home-indicator inset is the bar's to own — it pads itself, so the
   * glass runs to the bottom of the screen instead of floating above a strip
   * of background. Hence no 'bottom' edge here. */
  return (
    <SafeAreaView style={styles.container} edges={['top', 'left', 'right']}>
      <StatusBar barStyle="light-content" backgroundColor={colors.bg} />
      <BottomInsetProvider value={tabBarHeight}>
        <View style={styles.body}>
          {detailScreen ? (
            <EdgeSwipeBack onBack={() => guarded(closeDetail)}>
              {detailScreen}
            </EdgeSwipeBack>
          ) : (
            tabScreen
          )}
        </View>
        <TabBar
          tabs={TABS}
          active={tab}
          onSelect={next =>
            guarded(() => {
              setDetail(null);
              setTab(next);
            })
          }
        />
      </BottomInsetProvider>
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
