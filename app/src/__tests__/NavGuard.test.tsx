/**
 * The tab bar stays visible on detail screens, so tapping a tab is now the
 * easiest way to walk away from a half-edited config — and it never touches
 * the Back button the unsaved-changes confirmation is wired to. NavGuard is
 * what closes that hole; these tests pin it shut.
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
import { NavGuardProvider, useNavGuard } from '../ui/NavGuard';

function makeClient() {
  const client = {
    /* OutputsScreen now carries the live output switches, so it subscribes
     * to status the way the dashboard used to. */
    getLastStatus: jest.fn(() => null),
    onStatus: jest.fn(() => () => {}),
    setOutput: jest.fn(async () => {}),
    configRead: jest.fn(async () => defaultConfig()),
    configWrite: jest.fn(async (_cfg: DeviceConfig) => ({
      ok: true,
      result: 0,
      resultName: 'OK',
    })),
  } as unknown as MotoClient;
  return client;
}

function byLabel(tree: ReactTestRenderer, label: string): ReactTestInstance {
  return tree.root.find(
    n =>
      typeof n.type !== 'string' &&
      n.props.accessibilityLabel === label &&
      (!!n.props.onPress || !!n.props.onValueChange || !!n.props.onChangeText),
  );
}

/** Stands in for the tab bar: grabs `run` so a test can request navigation
 * exactly as a tab tap does. */
function makeHarness() {
  const ref: { run?: (proceed: () => void) => void } = {};
  function Probe(): null {
    const nav = useNavGuard();
    ref.run = nav?.run;
    return null;
  }
  return { ref, Probe };
}

describe('NavGuard', () => {
  const alertSpy = jest.spyOn(Alert, 'alert').mockImplementation(() => {});
  afterEach(() => alertSpy.mockClear());

  async function mount(onDone = jest.fn()) {
    const { ref, Probe } = makeHarness();
    let tree!: ReactTestRenderer;
    await act(async () => {
      tree = create(
        <NavGuardProvider>
          <Probe />
          <OutputsScreen client={makeClient()} onDone={onDone} />
        </NavGuardProvider>,
      );
    });
    return { tree, ref, onDone };
  }

  test('navigation goes straight through when nothing was edited', async () => {
    const { ref } = await mount();
    const navigated = jest.fn();

    act(() => ref.run?.(navigated));

    expect(alertSpy).not.toHaveBeenCalled();
    expect(navigated).toHaveBeenCalledTimes(1);
  });

  test('an edited screen holds the navigation until it is confirmed', async () => {
    const { tree, ref } = await mount();
    const navigated = jest.fn();

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Name').props.onChangeText('Heated Grips');
    });

    act(() => ref.run?.(navigated));

    /* Held, not cancelled — the rider is asked first. */
    expect(navigated).not.toHaveBeenCalled();
    expect(alertSpy).toHaveBeenCalledTimes(1);

    const buttons = alertSpy.mock.calls[0]![2]!;
    expect(buttons.map(b => b.text)).toEqual(['Keep editing', 'Discard']);

    act(() => buttons[1]!.onPress?.());
    expect(navigated).toHaveBeenCalledTimes(1);
  });

  test('keeping editing leaves the screen exactly where it was', async () => {
    const { tree, ref } = await mount();
    const navigated = jest.fn();

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Name').props.onChangeText('Heated Grips');
    });
    act(() => ref.run?.(navigated));

    /* "Keep editing" is a plain cancel: no onPress, so nothing runs. */
    const buttons = alertSpy.mock.calls[0]![2]!;
    act(() => buttons[0]!.onPress?.());

    expect(navigated).not.toHaveBeenCalled();
    expect(byLabel(tree, 'Name').props.value).toBe('Heated Grips');
  });

  /* The edge-swipe gesture is a second shell-driven exit that never touches
   * the Back chevron, so it has to run through the same guard — a swipe that
   * silently discarded a config would be worse than no gesture at all. This
   * pins the contract the shell relies on; the gesture recognition itself is
   * PanResponder and only observable on a device. */
  test('a shell-driven back is held the same way a tab tap is', async () => {
    const { tree, ref } = await mount();
    const closed = jest.fn();

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Name').props.onChangeText('Heated Grips');
    });

    act(() => ref.run?.(closed));

    expect(closed).not.toHaveBeenCalled();
    expect(alertSpy).toHaveBeenCalledTimes(1);
    act(() => alertSpy.mock.calls[0]![2]![1]!.onPress?.());
    expect(closed).toHaveBeenCalledTimes(1);
  });

  test('the guard is released when the screen unmounts', async () => {
    const { tree, ref } = await mount();

    await act(async () => {
      byLabel(tree, 'Output 3, On / off toggle').props.onPress();
    });
    await act(async () => {
      byLabel(tree, 'Name').props.onChangeText('Heated Grips');
    });
    await act(async () => {
      tree.unmount();
    });

    const navigated = jest.fn();
    act(() => ref.run?.(navigated));

    /* A dead screen must not keep vetoing navigation for the rest of the
     * session. */
    expect(alertSpy).not.toHaveBeenCalled();
    expect(navigated).toHaveBeenCalledTimes(1);
  });
});
