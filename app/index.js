/**
 * @format
 */
// Must be the first import: polyfills crypto.getRandomValues, which
// tweetnacl's CSPRNG requires and RN/Hermes doesn't provide natively.
import 'react-native-get-random-values';

// Hermes doesn't provide TextEncoder/TextDecoder either, which
// src/protocol/frames.ts needs for the wire protocol's base64/JSON framing.
import { TextEncoder, TextDecoder } from 'text-encoding';
if (typeof global.TextEncoder === 'undefined') {
  global.TextEncoder = TextEncoder;
}
if (typeof global.TextDecoder === 'undefined') {
  global.TextDecoder = TextDecoder;
}

import { AppRegistry } from 'react-native';
import App from './App';
import { name as appName } from './app.json';

/*
 * Starts BoardSession's watcher (src/ble/BoardSession.ts) as close to the
 * top of app boot as this file gets — deliberately before
 * AppRegistry.registerComponent, not inside a component effect. This is
 * what makes phone-as-key reconnect without the app open actually work:
 * when iOS relaunches the app in the background to hand back a
 * peripheral CoreBluetooth kept alive (state restoration,
 * src/ble/bleManager.ts), or when Android's BleWatchService brings the
 * process back, this file runs from scratch either way, and start()
 * needs to run every time, not just on a rider-initiated launch. Safe to
 * call unconditionally: it no-ops if there's no paired board yet, and is
 * idempotent if something (PairingScreen's own mount) calls it again.
 */
import { start as startBoardSession } from './src/ble/BoardSession';
startBoardSession();

AppRegistry.registerComponent(appName, () => App);
