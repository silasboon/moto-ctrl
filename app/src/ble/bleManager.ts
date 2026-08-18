/**
 * The one BleManager for the whole app, constructed with iOS state
 * restoration wired up from the start.
 *
 * Why this has to be its own module rather than living inside
 * BlePlxTransport (which used to lazily construct its own): CoreBluetooth's
 * background restoration only works if a CBCentralManager is created with
 * the same `restoreStateIdentifier` on every launch, including a launch iOS
 * triggers in the background to hand a reconnecting/reconnected peripheral
 * back to the app after it was fully terminated (not just suspended) —
 * `UIApplicationLaunchOptionsBluetoothCentralsKey`, see Info.plist's
 * UIBackgroundModes entry. There is no separate "did we restore" moment to
 * hook into after the fact: the manager has to exist, with these options,
 * before anything else touches BLE. index.js constructs it (via
 * BoardSession.start()) as close to the top of app boot as this codebase
 * gets, specifically so that holds true on a background relaunch too, not
 * just a normal foreground one.
 *
 * `restoreStateFunction` itself does not need to DO anything with the
 * restored peripherals for this to work: its only required job is to exist,
 * so ble-plx actually engages CBCentralManagerOptionRestoreIdentifierKey
 * natively. Whatever CoreBluetooth kept connected during restoration is
 * still there as far as the native peripheral object is concerned, and
 * BoardSession's own connect call (issued unconditionally on every boot,
 * restored or not) resolves immediately against an already-connected
 * peripheral rather than needing to know restoration happened at all — one
 * boot path for both cases, nothing restoration-specific to keep in sync.
 *
 * Android has no equivalent concept (GATT connections don't survive process
 * death the way a CBPeripheral does) — `restoreStateIdentifier` is simply
 * ignored there, and BoardSession's own foreground-service-backed retry loop
 * is what keeps Android's reconnect alive instead. See
 * android/app/src/main/java/com/motoctrl/app/ble/BleWatchService.kt.
 */
import { BleManager } from 'react-native-ble-plx';

const RESTORE_STATE_IDENTIFIER = 'moto-ctrl-central';

let manager: BleManager | null = null;

export function getBleManager(): BleManager {
  if (!manager) {
    manager = new BleManager({
      restoreStateIdentifier: RESTORE_STATE_IDENTIFIER,
      restoreStateFunction: () => {
        /* Intentionally a no-op beyond existing — see this file's header
         * comment for why nothing restoration-specific needs to happen
         * here. iOS only, and only fires on a background relaunch. */
      },
    });
  }
  return manager;
}
