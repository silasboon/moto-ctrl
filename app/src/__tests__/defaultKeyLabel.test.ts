/**
 * The default name a phone's key enrols under.
 *
 * Only Android can say anything real here — iOS 16 closed off the
 * user-assigned device name, so the honest ceiling there is the form factor.
 * These tests pin what each platform contributes, and specifically that the
 * brand isn't repeated when the model already carries it, which is the case
 * on most Samsung and Motorola devices.
 */
import { Platform } from 'react-native';

import { defaultKeyLabel } from '../identity/KeyStore';

function asAndroid(constants: { Brand?: string; Model?: string }): void {
  Object.defineProperty(Platform, 'OS', { value: 'android', writable: true });
  Object.defineProperty(Platform, 'constants', {
    value: constants,
    writable: true,
  });
}

function asIos(interfaceIdiom: string): void {
  Object.defineProperty(Platform, 'OS', { value: 'ios', writable: true });
  Object.defineProperty(Platform, 'constants', {
    value: { interfaceIdiom },
    writable: true,
  });
}

describe('defaultKeyLabel', () => {
  const realOS = Platform.OS;
  const realConstants = Platform.constants;
  afterEach(() => {
    Object.defineProperty(Platform, 'OS', { value: realOS, writable: true });
    Object.defineProperty(Platform, 'constants', {
      value: realConstants,
      writable: true,
    });
  });

  test('Android combines brand and model', () => {
    asAndroid({ Brand: 'Google', Model: 'Pixel 8' });
    expect(defaultKeyLabel()).toBe('Google Pixel 8');
  });

  test('Android does not repeat a brand the model already carries', () => {
    asAndroid({ Brand: 'samsung', Model: 'Samsung SM-G991B' });
    expect(defaultKeyLabel()).toBe('Samsung SM-G991B');
  });

  test('Android falls back when the model is missing', () => {
    asAndroid({ Brand: 'Google', Model: '' });
    expect(defaultKeyLabel()).toBe('Android phone');
  });

  /* Not "Silas's iPhone" — that needs an Apple entitlement this app has no
   * grounds to request, which is exactly why the label is editable. */
  test('iOS gives the form factor and nothing more', () => {
    asIos('phone');
    expect(defaultKeyLabel()).toBe('iPhone');
    asIos('pad');
    expect(defaultKeyLabel()).toBe('iPad');
  });
});
