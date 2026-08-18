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
import React, { useCallback, useEffect, useRef, useState } from 'react';
import { StatusBar, StyleSheet, View } from 'react-native';
import { SafeAreaProvider, SafeAreaView } from 'react-native-safe-area-context';

import { colors } from './src/ui/theme';
import { EdgeSwipeBack } from './src/ui/EdgeSwipeBack';
import { NavGuardProvider, useNavGuard } from './src/ui/NavGuard';

import {
  getState as getBoardState,
  onStateChange as onBoardStateChange,
  stop as stopBoardSession,
  type BoardSessionState,
} from './src/ble/BoardSession';
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

type Detail =
  | 'board'
  | 'outputs'
  | 'buttons'
  | 'keys'
  | 'lock'
  | 'diagnostics'
  | 'firmwareUpdate'
  | 'eventLog';

function AppContent(): React.JSX.Element {
  const nav = useNavGuard();
  /* BoardSession (src/ble/BoardSession.ts) is the single source of truth
   * for "are we connected" — it keeps running whether or not this
   * component is even mounted (background reconnect is the whole point),
   * so this is a subscription to shared state, not state this component
   * owns. getBoardState() as the initial value (rather than 'idle') is
   * what lets a session already established before first render —
   * restored from an iOS background relaunch, or the watcher having beaten
   * this render to a connection — show up immediately instead of flashing
   * PairingScreen first. */
  const [boardState, setBoardState] = useState<BoardSessionState>(getBoardState);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [detail, setDetail] = useState<Detail | null>(null);
  /* Surfaced on the pairing screen after an unexpected drop, so a board going
   * out of range reads as a plain message rather than whatever error the
   * in-flight request happened to reject with. Derived from a 'connected' ->
   * 'watching' (reason 'lost') transition below, using the device name from
   * the state we're leaving, since the new one no longer carries it. */
  const [notice, setNotice] = useState<string | null>(null);
  const lastDeviceNameRef = useRef<string | null>(null);

  useEffect(() => {
    return onBoardStateChange(next => {
      if (next.type === 'connected') {
        setNotice(null);
        lastDeviceNameRef.current = next.device.name;
        setSettingsOpen(false);
        setDetail(null);
      } else if (next.type === 'watching' && next.reason === 'lost') {
        const name = lastDeviceNameRef.current;
        setNotice(
          name
            ? `Lost connection to ${name}. Searching for it again…`
            : 'Lost connection. Searching for it again…',
        );
        setSettingsOpen(false);
        setDetail(null);
      }
      setBoardState(next);
    });
  }, []);

  // Ownership transfer wipes every enrolled key (including this phone's) and
  // resets lock config device-side — treat it like a disconnect back to
  // Pairing. If this is the same phone, PairingScreen's existing
  // authenticate-then-fall-back-to-enroll flow re-enrolls it via
  // trust-on-first-use, since the keystore is now empty.
  const handleDisconnect = useCallback(() => {
    setNotice(null);
    void stopBoardSession();
  }, []);
  const handleOwnershipTransferred = handleDisconnect;

  if (boardState.type !== 'connected') {
    return (
      <SafeAreaView style={styles.container}>
        <StatusBar barStyle="light-content" backgroundColor={colors.bg} />
        <PairingScreen notice={notice} />
      </SafeAreaView>
    );
  }

  const client = boardState.client;
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
            deviceName={boardState.device.name}
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
