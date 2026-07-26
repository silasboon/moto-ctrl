# Native project setup (ios/ and android/)

This is a **bare** React Native app (no Expo), which normally means the
`ios/` and `android/` native projects are generated once and committed
alongside the JS/TS source.

They are **not generated yet** in this scaffold. Bare RN's native
project templates (Xcode project files, Podfile, Gradle files, wrapper
jars, `AndroidManifest.xml`, etc.) are large, mostly-binary-ish,
version-coupled artifacts — generating them correctly requires actually
running the React Native CLI with a clean npm environment, not hand-writing
them. That step is deferred to whoever first needs a runnable-on-device
build, with a working local toolchain (Xcode + CocoaPods + Android SDK).

## To generate them

From the repo root:

```sh
tools/bootstrap-app-native.sh
```

This runs the pinned React Native CLI `init` in a scratch directory with the
exact `react-native` version from `app/package.json`, then copies only the
generated `ios/` and `android/` directories into `app/`, so the JS/TS source
already in `app/` is untouched. Review the diff before committing — in
particular, confirm the bundle identifier / application ID matches what the
project wants (currently unset; ask before choosing one, since it's
effectively permanent once published to app stores).

After bootstrapping:

```sh
cd app
npm install
npx pod-install ios   # iOS only, requires CocoaPods
npm run ios            # or: npm run android
```

## Why this matters

The app can't produce a runnable-on-device build without this step.
Bench/CI testing against `firmware/sim/` (TypeScript + Metro + Jest) does
not require it — only `npm run ios` / `npm run android` do.
