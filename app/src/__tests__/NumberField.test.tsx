/**
 * A number field has to be emptyable while it is being edited.
 *
 * Driving a TextInput straight from its numeric value makes it impossible to
 * backspace: deleting the last digit yields "", which parses to NaN, falls
 * back to a minimum, and is re-rendered as a digit — so the field appears to
 * refuse the delete. These tests pin the draft behaviour that fixes it, and
 * the two properties that make an empty draft safe: nothing is committed
 * while the field is empty, and blurring restores the last committed value
 * rather than leaving a phantom 0 behind.
 */
import React from 'react';
import { TextInput } from 'react-native';
import {
  act,
  create,
  type ReactTestInstance,
  type ReactTestRenderer,
} from 'react-test-renderer';

import { NumberField } from '../ui/components';

function field(tree: ReactTestRenderer): ReactTestInstance {
  return tree.root.findByType(TextInput);
}

/** Renders a NumberField wired to state, the way a screen uses it. */
function mount(props: {
  initial: number;
  min?: number;
  max?: number;
  decimal?: boolean;
}) {
  const commits: number[] = [];
  function Host(): React.JSX.Element {
    const [value, setValue] = React.useState(props.initial);
    return (
      <NumberField
        label="Blink period (ms)"
        value={value}
        min={props.min}
        max={props.max}
        decimal={props.decimal}
        onChangeValue={v => {
          commits.push(v);
          setValue(v);
        }}
      />
    );
  }
  let tree!: ReactTestRenderer;
  act(() => {
    tree = create(<Host />);
  });
  return { tree, commits };
}

function type(tree: ReactTestRenderer, text: string): void {
  act(() => {
    field(tree).props.onChangeText(text);
  });
}

describe('NumberField', () => {
  test('can be cleared, and commits nothing while empty', () => {
    const { tree, commits } = mount({ initial: 500, min: 1 });

    type(tree, '50');
    type(tree, '5');
    type(tree, '');

    expect(field(tree).props.value).toBe('');
    /* 5 and 50 are real intermediate values; the empty field is not. */
    expect(commits).toEqual([50, 5]);
  });

  test('typing a fresh value after clearing works', () => {
    const { tree, commits } = mount({ initial: 500, min: 1 });

    type(tree, '');
    type(tree, '9');
    type(tree, '90');

    expect(field(tree).props.value).toBe('90');
    expect(commits[commits.length - 1]).toBe(90);
  });

  test('blurring an emptied field restores the last committed value', () => {
    const { tree, commits } = mount({ initial: 500, min: 1 });

    type(tree, '');
    act(() => {
      field(tree).props.onBlur();
    });

    expect(field(tree).props.value).toBe('500');
    expect(commits).toEqual([]); // never saved a phantom 0
  });

  test('clamps to the configured range', () => {
    const { tree, commits } = mount({ initial: 100, min: 1, max: 100 });

    type(tree, '250');
    expect(commits[commits.length - 1]).toBe(100);

    type(tree, '0');
    expect(commits[commits.length - 1]).toBe(1);
  });

  test('rejects characters a number pad should never have produced', () => {
    const { tree } = mount({ initial: 12, min: 0 });

    type(tree, '1a2,');
    expect(field(tree).props.value).toBe('12');
  });

  test('holds a half-typed decimal without committing it', () => {
    const { tree, commits } = mount({ initial: 1.5, decimal: true, min: 0 });

    type(tree, '2.');
    expect(field(tree).props.value).toBe('2.');
    expect(commits).toEqual([2]); // "2." parses as 2; the dot is still pending

    type(tree, '2.25');
    expect(commits[commits.length - 1]).toBe(2.25);
  });

  test('a minus sign is only offered where negatives make sense', () => {
    const unsigned = mount({ initial: 5, min: 0 });
    type(unsigned.tree, '-5');
    expect(field(unsigned.tree).props.value).toBe('5');
    expect(field(unsigned.tree).props.keyboardType).toBe('number-pad');

    /* Calibration offsets have no min — a sense line can read low or high. */
    const signed = mount({ initial: 5 });
    type(signed.tree, '-5');
    expect(field(signed.tree).props.value).toBe('-5');
    expect(signed.commits[signed.commits.length - 1]).toBe(-5);
    expect(field(signed.tree).props.keyboardType).toBe('numeric');
  });
});
