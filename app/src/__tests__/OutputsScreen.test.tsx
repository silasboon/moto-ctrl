/**
 * Render tests for OutputsScreen. Narrow on purpose: they prove the screen
 * writes the exact schema-6 shape the firmware expects, and that the two
 * at-most-one roles can't be set on two channels at once — the firmware
 * rejects such a config outright, so the UI must not let you build one.
 */
import React from 'react';
import {
  act,
  create,
  type ReactTestInstance,
  type ReactTestRenderer,
} from 'react-test-renderer';

import { OutputsScreen } from '../screens/OutputsScreen';
import type { MotoClient } from '../protocol/MotoClient';
import { defaultConfig, type DeviceConfig } from '../protocol/types';

function makeClient(initial?: DeviceConfig) {
  const calls = { writes: [] as DeviceConfig[] };
  const client = {
    configRead: jest.fn(async () => initial ?? defaultConfig()),
    configWrite: jest.fn(async (cfg: DeviceConfig) => {
      calls.writes.push(JSON.parse(JSON.stringify(cfg)) as DeviceConfig);
      return { ok: true, result: 0, resultName: 'OK' };
    }),
  } as unknown as MotoClient;
  return { client, calls };
}

function byLabel(tree: ReactTestRenderer, label: string): ReactTestInstance {
  return tree.root.find(
    n =>
      typeof n.type !== 'string' &&
      n.props.accessibilityLabel === label &&
      (!!n.props.onPress || !!n.props.onValueChange || !!n.props.onChangeText),
  );
}

async function mount(client: MotoClient) {
  let tree!: ReactTestRenderer;
  await act(async () => {
    tree = create(<OutputsScreen client={client} onDone={jest.fn()} />);
  });
  return tree;
}

describe('OutputsScreen', () => {
  test('writes the schema-6 channel shape, with no legacy keys', async () => {
    const { client, calls } = makeClient();
    const tree = await mount(client);
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    const written = calls.writes[0]!;
    expect(written.schema_version).toBe(6);
    const ch = written.outputs.channels[0]!;
    expect(ch).toMatchObject({
      name: '',
      behaviour: 'toggle',
      essential: false,
      is_ignition: false,
      is_starter: false,
      is_brake: false,
      indicator: 'none',
      hazard_member: false,
    });
    // The v5 taxonomy must be gone from the wire entirely.
    expect(ch).not.toHaveProperty('function');
    expect(ch).not.toHaveProperty('mode');
    expect(ch).not.toHaveProperty('momentary');
  });

  test('marking a second channel as starter clears the first', async () => {
    const cfg = defaultConfig();
    cfg.outputs.channels[2]!.is_starter = true;
    const { client, calls } = makeClient(cfg);
    const tree = await mount(client);

    // Open channel 5 and mark it the starter.
    await act(async () => {
      byLabel(tree, 'Output 5, On / off toggle').props.onPress();
    });
    await act(async () => {
      // ToggleRow exposes the whole row as one control; pressing it toggles.
      byLabel(tree, 'Starter').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    const written = calls.writes[0]!;
    const starters = written.outputs.channels.filter(c => c.is_starter);
    expect(starters).toHaveLength(1);
    expect(written.outputs.channels[4]!.is_starter).toBe(true);
    expect(written.outputs.channels[2]!.is_starter).toBe(false);
  });

  /* Ignition and brake are essential whether or not the flag is ticked, so the
   * switch shows on and is locked — presenting it as togglable would imply you
   * can shed your ignition, which AGENTS.md #1 forbids. */
  test('essential is forced on and locked for ignition and brake channels', async () => {
    const cfg = defaultConfig();
    cfg.outputs.channels[0]!.is_ignition = true;
    const { client } = makeClient(cfg);
    const tree = await mount(client);

    await act(async () => {
      byLabel(tree, 'Output 1, On / off toggle').props.onPress();
    });
    const essential = byLabel(tree, 'Essential');
    expect(essential.props.accessibilityState.checked).toBe(true);
    expect(essential.props.accessibilityState.disabled).toBe(true);
  });

  test('a renamed channel keeps its name in the written config', async () => {
    const { client, calls } = makeClient();
    const tree = await mount(client);
    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Name').props.onChangeText('Heated Grips');
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });
    expect(calls.writes[0]!.outputs.channels[2]!.name).toBe('Heated Grips');
  });
});
