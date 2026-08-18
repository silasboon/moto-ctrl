/**
 * Thin wrapper around the Android-only BleWatchService native module
 * (android/app/src/main/java/com/motoctrl/app/ble/) — the foreground
 * service that keeps the app process alive in the background so
 * BoardSession's reconnect watcher isn't frozen/killed by Android's
 * background-execution limits.
 *
 * No iOS equivalent: `NativeModules.BleWatchService` is simply absent
 * there (CoreBluetooth's own background-restoration mechanism, wired up in
 * bleManager.ts, is what does the equivalent job on that platform), and
 * also absent under Jest (no native modules at all). Both calls degrade to
 * a no-op rather than throwing — BoardSession's state machine calls these
 * unconditionally on every transition rather than checking Platform.OS
 * itself, so this is the one place that needs to know the module might not
 * exist.
 */
import { NativeModules, Platform } from 'react-native';

interface BleWatchServiceNative {
  ensureRunning(label: string): void;
  stop(): void;
}

const native: BleWatchServiceNative | undefined =
  Platform.OS === 'android'
    ? (NativeModules.BleWatchService as BleWatchServiceNative | undefined)
    : undefined;

/** Starts the foreground service (or just updates its notification text if
 * already running) with the given rider-facing label. */
export function ensureWatchServiceRunning(label: string): void {
  native?.ensureRunning(label);
}

/** Stops the foreground service — BoardSession calls this once there is no
 * known device to watch for, or on an explicit rider Disconnect. */
export function stopWatchService(): void {
  native?.stop();
}
