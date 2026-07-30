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
import nacl from 'tweetnacl';

import { base64ToBytes, bytesToBase64 } from '../protocol/frames';
import type { Keypair } from '../protocol/types';

const IDENTITY_KEY = 'motoctrl.identity.v1';
const LAST_DEVICE_KEY = 'motoctrl.lastDevice.v1';
const DEFAULT_LABEL = 'My Phone';

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
    label: DEFAULT_LABEL,
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
