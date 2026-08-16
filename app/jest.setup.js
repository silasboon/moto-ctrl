/**
 * Runs before each test file's own top-level code (Jest `setupFiles`), so it
 * lands before any import chain reaches src/platform/webSocketGlobal.ts,
 * which captures the bare `WebSocket` identifier at module-load time. In the
 * real app that's React Native's own runtime global; Jest's test environment
 * (jest-environment-node, via @react-native/jest-preset) never provides one.
 *
 * Node itself has shipped a built-in global `WebSocket` (via undici) since
 * v22, so this was silently masked on newer local Node versions — tests
 * passed locally while failing on CI's pinned Node 20 with `ReferenceError:
 * WebSocket is not defined`. Polyfilling explicitly here removes the
 * dependency on whichever Node version happens to be running the tests.
 */
global.WebSocket = require('ws');
