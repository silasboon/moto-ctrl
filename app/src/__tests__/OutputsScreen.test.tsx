/**
 * Render tests for OutputsScreen. Narrow on purpose: they prove the screen
 * writes the exact schema-8 shape the firmware expects, and that the
 * constraints the firmware validates — at-most-one ignition/starter,
 * reciprocal alternating pairs — can't be violated from the UI at all. A
 * config that only fails on save is a worse experience than one the screen
 * refuses to build.
 */
import React from 'react';
import { Alert } from 'react-native';
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
    /* OutputsScreen now carries the live output switches, so it subscribes
     * to status the way the dashboard used to. */
    getLastStatus: jest.fn(() => null),
    onStatus: jest.fn(() => () => {}),
    setOutput: jest.fn(async () => {}),
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
  test('writes the schema-8 channel shape, with no legacy keys', async () => {
    const { client, calls } = makeClient();
    const tree = await mount(client);
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    const written = calls.writes[0]!;
    expect(written.schema_version).toBe(8);
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
      on_with_ignition: false,
      alternate_channel: -1,
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

  /* An asymmetric pair is a config the firmware rejects outright
   * (MC_OUT_CFG_BAD_ALTERNATE), and the failure mode it prevents is both
   * beams lit at once — so the screen writes both sides of a pair together
   * rather than letting the rider build a half-link. */
  test('pairing two channels writes the link on both of them', async () => {
    const { client, calls } = makeClient();
    const tree = await mount(client);

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Alternates with: None').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Output 5').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    const channels = calls.writes[0]!.outputs.channels;
    expect(channels[2]!.alternate_channel).toBe(4);
    expect(channels[4]!.alternate_channel).toBe(2);
  });

  test('re-pairing a channel releases the partner it is leaving', async () => {
    const cfg = defaultConfig();
    cfg.outputs.channels[2]!.alternate_channel = 4;
    cfg.outputs.channels[4]!.alternate_channel = 2;
    const { client, calls } = makeClient(cfg);
    const tree = await mount(client);

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Alternates with: Output 5').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'None').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    const channels = calls.writes[0]!.outputs.channels;
    expect(channels[2]!.alternate_channel).toBe(-1);
    /* The abandoned partner must be released too, or it keeps a one-way
     * link to a channel that no longer points back. */
    expect(channels[4]!.alternate_channel).toBe(-1);
  });

  /* Everything on this screen is local until Save, and re-entering a set of
   * channel names and roles is minutes of work — leaving must not throw it
   * away silently. */
  describe('leaving with unsaved edits', () => {
    const alertSpy = jest.spyOn(Alert, 'alert').mockImplementation(() => {});
    afterEach(() => alertSpy.mockClear());

    async function mountWith(onDone: () => void) {
      const { client } = makeClient();
      let tree!: ReactTestRenderer;
      await act(async () => {
        tree = create(<OutputsScreen client={client} onDone={onDone} />);
      });
      return { tree, client };
    }

    test('back leaves straight away when nothing was edited', async () => {
      const onDone = jest.fn();
      const { tree } = await mountWith(onDone);

      await act(async () => {
        byLabel(tree, 'Back').props.onPress();
      });

      expect(alertSpy).not.toHaveBeenCalled();
      expect(onDone).toHaveBeenCalledTimes(1);
    });

    test('back asks first when there are edits, and stays put', async () => {
      const onDone = jest.fn();
      const { tree } = await mountWith(onDone);

      await act(async () => {
        byLabel(tree, 'Output 3, On / off toggle').props.onPress();
      });
      await act(async () => {
        byLabel(tree, 'Name').props.onChangeText('Heated Grips');
      });
      await act(async () => {
        byLabel(tree, 'Back').props.onPress();
      });

      expect(onDone).not.toHaveBeenCalled();
      expect(alertSpy).toHaveBeenCalledTimes(1);

      /* Confirming discards; the buttons are [Keep editing, Discard]. */
      const buttons = alertSpy.mock.calls[0]![2]!;
      expect(buttons.map(b => b.text)).toEqual(['Keep editing', 'Discard']);
      act(() => {
        buttons[1]!.onPress?.();
      });
      expect(onDone).toHaveBeenCalledTimes(1);
    });

    test('a saved screen is clean again', async () => {
      const onDone = jest.fn();
      const { tree } = await mountWith(onDone);

      await act(async () => {
        byLabel(tree, 'Output 3, On / off toggle').props.onPress();
      });
      await act(async () => {
        byLabel(tree, 'Name').props.onChangeText('Heated Grips');
      });
      await act(async () => {
        byLabel(tree, 'Save').props.onPress();
      });
      await act(async () => {
        byLabel(tree, 'Back').props.onPress();
      });

      expect(alertSpy).not.toHaveBeenCalled();
      expect(onDone).toHaveBeenCalledTimes(1);
    });
  });
});
