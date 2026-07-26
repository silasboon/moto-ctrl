import { BlePlxTransport } from '../transport/BlePlxTransport';
import { SimTransport } from '../transport/SimTransport';
import type { Transport } from '../transport/Transport';

describe('Transport implementations (scaffold stage)', () => {
  test.each<[string, Transport]>([
    ['BlePlxTransport', new BlePlxTransport()],
    ['SimTransport', new SimTransport('ws://localhost:9000')],
  ])('%s starts disconnected', (_name, transport) => {
    expect(transport.getConnectionState()).toBe('disconnected');
  });
});
