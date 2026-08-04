module.exports = {
  preset: '@react-native/jest-preset',
  /* AsyncStorage and liquid-glass both ship untranspiled ESM, so they have to
   * go through babel like the react-native packages do. */
  transformIgnorePatterns: [
    'node_modules/(?!(react-native|@react-native|react-native-ble-plx|@react-native-async-storage|@callstack/liquid-glass)/)',
  ],
};
