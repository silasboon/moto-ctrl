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

AppRegistry.registerComponent(appName, () => App);
