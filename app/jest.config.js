module.exports = {
  preset: '@react-native/jest-preset',
  /* AsyncStorage and liquid-glass both ship untranspiled ESM, so they have to
   * go through babel like the react-native packages do. */
  transformIgnorePatterns: [
    'node_modules/(?!(react-native|@react-native|react-native-ble-plx|@react-native-async-storage|@callstack/liquid-glass)/)',
  ],
  setupFiles: ['<rootDir>/jest.setup.js'],
  /* Default (5000ms) is tight for tests that mount a full screen via
   * react-test-renderer (OutputsScreen and anything that mounts it, e.g.
   * NavGuard.test.tsx) — fine on a quiet machine, but a shared/loaded CI
   * runner can tip one over and fail on pure timing, not a real hang. */
  testTimeout: 15000,
};
