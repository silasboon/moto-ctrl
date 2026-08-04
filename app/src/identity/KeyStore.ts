/**
 * Persists this phone's Ed25519 identity (its "key" for phone-as-key
 * pairing, AGENTS.md safety requirement #4) and the last-paired device.
 *
 * Storage: @react-native-async-storage/async-storage — plain local
 * storage, offline-first, nothing leaves the phone. This is a deliberate
 * MVP tradeoff, decided explicitly rather than assumed: the private key
 * sits in AsyncStorage (not the iOS Keychain / Android Keystore) for now.
 * Moving it to platform secure storage remains a known follow-up — see
 * docs/TESTING.md.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import { Platform } from 'react-native';
import nacl from 'tweetnacl';

import { base64ToBytes, bytesToBase64 } from '../protocol/frames';
import type { Keypair } from '../protocol/types';

const IDENTITY_KEY = 'motoctrl.identity.v1';
const LAST_DEVICE_KEY = 'motoctrl.lastDevice.v1';

/**
 * A starting label for this phone's key, from what the platform will tell us
 * for free.
 *
 * This is NOT the phone's user-assigned name ("Silas's iPhone"). iOS 16
 * closed that off: `UIDevice.name` returns the model unless the app carries
 * the user-assigned-device-name entitlement, which has to be requested from
 * Apple with a justification, and a board's key list is not one. A native
 * device-info dependency would hit the same wall, so there is nothing to buy
 * here — hence no dependency and no pretending.
 *
 * Android does expose the real brand and model through Platform.constants, so
 * it gets "Google Pixel 8" where iOS gets "iPhone". Either way this is only a
 * default: the rider can edit it before enrolling, which is the only route to
 * a personal name on iOS and the only thing that helps when two iPhones share
 * a bike.
 */
export function defaultKeyLabel(): string {
  if (Platform.OS === 'android') {
    const { Brand, Model } = Platform.constants;
    const brand = (Brand ?? '').trim();
    const model = (Model ?? '').trim();
    if (model) {
      /* Manufacturers often already prefix the model with the brand
       * ("Samsung SM-G991B"), so don't say it twice. */
      const prefixed =
        brand && !model.toLowerCase().startsWith(brand.toLowerCase());
      return prefixed ? `${brand} ${model}` : model;
    }
    return 'Android phone';
  }
  if (Platform.OS === 'ios') {
    return Platform.constants.interfaceIdiom === 'pad' ? 'iPad' : 'iPhone';
  }
  return 'My Phone';
}

export interface Identity {
  keypair: Keypair;
  label: string;
}

interface StoredIdentity {
  publicKey: string;
  secretKey: string;
  label: string;
}

export async function loadOrCreateIdentity(): Promise<Identity> {
  const raw = await AsyncStorage.getItem(IDENTITY_KEY);
  if (raw) {
    const parsed = JSON.parse(raw) as StoredIdentity;
    return {
      keypair: {
        publicKey: base64ToBytes(parsed.publicKey),
        secretKey: base64ToBytes(parsed.secretKey),
      },
      label: parsed.label,
    };
  }
  const kp = nacl.sign.keyPair();
  const identity: Identity = {
    keypair: { publicKey: kp.publicKey, secretKey: kp.secretKey },
    label: defaultKeyLabel(),
  };
  await saveIdentity(identity);
  return identity;
}

export async function saveIdentity(identity: Identity): Promise<void> {
  const stored: StoredIdentity = {
    publicKey: bytesToBase64(identity.keypair.publicKey),
    secretKey: bytesToBase64(identity.keypair.secretKey),
    label: identity.label,
  };
  await AsyncStorage.setItem(IDENTITY_KEY, JSON.stringify(stored));
}

export interface LastDevice {
  id: string;
  name: string;
}

export async function loadLastDevice(): Promise<LastDevice | null> {
  const raw = await AsyncStorage.getItem(LAST_DEVICE_KEY);
  return raw ? (JSON.parse(raw) as LastDevice) : null;
}

export async function saveLastDevice(device: LastDevice): Promise<void> {
  await AsyncStorage.setItem(LAST_DEVICE_KEY, JSON.stringify(device));
}
