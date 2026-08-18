# Native project setup (ios/ and android/)

This is a **bare** React Native app (no Expo), so the `ios/` and `android/`
native projects are generated once and committed alongside the JS/TS
source. They are committed here — you do not need to generate anything to
build the app.

Both platforms are already configured for what this app needs:

- Bundle identifier / application ID: `com.motoctrl.app`.
- BLE permissions declared. iOS uses `NSBluetoothAlwaysUsageDescription`;
  Android declares `BLUETOOTH_SCAN` (with `neverForLocation`) and
  `BLUETOOTH_CONNECT` for API 31+, plus the legacy
  `BLUETOOTH`/`BLUETOOTH_ADMIN` permissions capped at `maxSdkVersion="30"`.
- Background BLE reconnect (`src/ble/BoardSession.ts`; see
  [`README.md`](README.md) for the full picture). iOS: `UIBackgroundModes`
  → `bluetooth-central` in Info.plist. Android: `BleWatchService`, a small
  custom native module and foreground service at
  `android/app/src/main/java/com/motoctrl/app/ble/`, registered in
  `MainApplication.kt`'s package list, with its own manifest permissions
  (`FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_CONNECTED_DEVICE`,
  `POST_NOTIFICATIONS`) and `<service>` declaration.

## Running on a device or simulator

```sh
cd app
npm install
npx pod-install ios   # iOS only, requires CocoaPods
npm run ios            # or: npm run android
```

iOS needs Xcode and CocoaPods; Android needs the Android SDK. BLE will not
work on the iOS Simulator or a stock Android emulator — scanning for a
board requires a physical device.

Nothing in the test path needs any of this. Typecheck, lint, and the Jest
suites (including the live `firmware/sim/` integration test) run on the
JS/TS source alone — see [`README.md`](README.md).

## Regenerating the native projects

Only needed if you're upgrading React Native to a version whose template
changed, or repairing a broken native project:

```sh
tools/bootstrap-app-native.sh
```

This runs the pinned React Native CLI `init` in a scratch directory with the
exact `react-native` version from `app/package.json`, then copies the
generated `ios/` and `android/` directories into `app/`, leaving the JS/TS
source untouched.

**Review the diff carefully before committing.** Regenerating overwrites
the local changes listed above — the bundle identifier / application ID,
the BLE permission declarations, and the entire background-reconnect setup
(Info.plist's `UIBackgroundModes`, and Android's `BleWatchService` +
`MainApplication.kt` registration + manifest entries, none of which are
part of the stock template) must be re-applied by hand if the regenerated
project drops them. The `android/app/src/main/java/com/motoctrl/app/ble/`
directory itself won't be touched by the regeneration (it's outside what
the RN CLI template writes), but `MainApplication.kt` and
`AndroidManifest.xml` will be, so double-check
`BleWatchPackage()`/permissions/`<service>` are still there afterward.
Changing the bundle identifier is effectively permanent once an app has
been published, so don't let a regeneration silently reset it either.
