/**
 * Render-level tests for ButtonsScreen against a stub MotoClient.
 *
 * Deliberately narrow: these prove the screen mounts, that identify mode is
 * turned on and — critically — turned back OFF when the screen goes away, and
 * that a binding edit produces the exact config the device expects. They do
 * not re-verify protocol semantics; firmware/sim/tests and the sim itest own
 * that.
 *
 * The unmount case matters more than it looks: learn mode makes the board push
 * a frame on every handlebar press, so a screen that forgets to disable it
 * leaves the radio chattering for the rest of the ride (AGENTS.md #7).
 */
import React from 'react';
import {
  act,
  create,
  type ReactTestInstance,
  type ReactTestRenderer,
} from 'react-test-renderer';

import { ButtonsScreen } from '../screens/ButtonsScreen';
import type { InputEvent, MotoClient } from '../protocol/MotoClient';
import {
  ACTION_OUTPUT_TOGGLE_BASE,
  defaultConfig,
  type DeviceConfig,
} from '../protocol/types';

function makeClient(overrides: Partial<MotoClient> = {}) {
  const listeners = new Set<(e: InputEvent) => void>();
  const calls = { learn: [] as boolean[], writes: [] as DeviceConfig[] };
  const client = {
    configRead: jest.fn(async () => defaultConfig()),
    configWrite: jest.fn(async (cfg: DeviceConfig) => {
      calls.writes.push(JSON.parse(JSON.stringify(cfg)) as DeviceConfig);
      return { ok: true, result: 0, resultName: 'OK' };
    }),
    inputLearn: jest.fn(async (enable: boolean) => {
      calls.learn.push(enable);
      return { ok: true, result: 0, resultName: 'OK' };
    }),
    onInputEvent: (l: (e: InputEvent) => void) => {
      listeners.add(l);
      return () => listeners.delete(l);
    },
    ...overrides,
  } as unknown as MotoClient;
  const emit = (e: InputEvent) => {
    for (const l of listeners) l(e);
  };
  return { client, calls, emit };
}

/** Finds a node by its accessibilityLabel, or the exact text it renders. */
function byLabel(tree: ReactTestRenderer, label: string): ReactTestInstance {
  return tree.root.find(
    n =>
      typeof n.type !== 'string' &&
      n.props.accessibilityLabel === label &&
      !!n.props.onPress,
  );
}
function texts(tree: ReactTestRenderer): string[] {
  return tree.root.findAllByType('Text' as never).flatMap(n => {
    const c = n.props.children;
    return typeof c === 'string' ? [c] : [];
  });
}

async function mountScreen(client: MotoClient, onDone = jest.fn()) {
  let tree!: ReactTestRenderer;
  await act(async () => {
    tree = create(<ButtonsScreen client={client} onDone={onDone} />);
  });
  return tree;
}

describe('ButtonsScreen', () => {
  test('renders all 8 inputs once the config loads', async () => {
    const { client } = makeClient();
    const tree = await mountScreen(client);
    const labels = texts(tree);
    for (let i = 1; i <= 8; i++) {
      expect(labels).toContain(`Button ${i}`);
    }
    expect(client.configRead).toHaveBeenCalled();
  });

  test('identify mode is enabled on request and disabled again on unmount', async () => {
    const { client, calls } = makeClient();
    const tree = await mountScreen(client);

    await act(async () => {
      byLabel(tree, 'Start identify mode').props.onPress();
    });
    expect(calls.learn).toEqual([true]);

    // Navigating away must not leave the board pushing press events.
    await act(async () => {
      tree.unmount();
    });
    expect(calls.learn).toEqual([true, false]);
  });

  test('does not disable identify mode on unmount if it was never enabled', async () => {
    const { client, calls } = makeClient();
    const tree = await mountScreen(client);
    await act(async () => {
      tree.unmount();
    });
    expect(calls.learn).toEqual([]);
  });

  test('a pushed press event names the button that was pressed', async () => {
    const { client, emit } = makeClient();
    const tree = await mountScreen(client);
    await act(async () => {
      byLabel(tree, 'Start identify mode').props.onPress();
    });
    await act(async () => {
      emit({ button: 4, pressType: 'double', actionSuppressed: false });
    });
    const labels = texts(tree);
    expect(labels).toContain('INPUT 5'); // 0-indexed on the wire, 1-indexed in UI
    expect(labels).toContain('DOUBLE');
  });

  test('binding a single press writes an action list for that button', async () => {
    const { client, calls } = makeClient();
    const tree = await mountScreen(client);

    // Open button 3, start editing its single press, bind Output 1.
    await act(async () => {
      byLabel(tree, 'Button 3, 0 bindings').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Edit Single press for Button 3').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Output 1').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    expect(calls.writes).toHaveLength(1);
    const written = calls.writes[0]!;
    // Button 3 in the UI is input index 2 on the wire.
    expect(written.inputs.short_press_action[2]).toEqual([
      ACTION_OUTPUT_TOGGLE_BASE + 0,
    ]);
    // Every other button stays unbound, and the arrays stay positional.
    expect(written.inputs.short_press_action).toHaveLength(8);
    expect(written.inputs.short_press_action[0]).toEqual([]);
  });

  test('an incomplete chord is refused before it reaches the device', async () => {
    const { client, calls } = makeClient();
    const tree = await mountScreen(client);

    await act(async () => {
      byLabel(tree, 'Add chord').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Save').props.onPress();
    });

    expect(calls.writes).toHaveLength(0);
    expect(
      texts(tree).some(t => t.includes('needs at least two buttons')),
    ).toBe(true);
  });
});
