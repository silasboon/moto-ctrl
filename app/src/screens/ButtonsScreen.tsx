/**
 * Buttons: identify the 8 handlebar inputs, name them, and bind each one's
 * single / double / hold press to one or more outputs. Also edits chords —
 * several buttons pressed together driving a set of outputs.
 *
 * All 8 inputs and all 12 outputs are electrically identical, so this screen
 * binds a button straight to an output CHANNEL (action id 256 + N, see
 * docs/PROTOCOL.md §9) rather than to a semantic function. The three
 * function-based ids (turn L/R, hazards) are still offered, because those
 * carry flasher behaviour a plain toggle doesn't.
 *
 * Identify flow: MC_OP_INPUT_LEARN turns on a per-session push of every
 * debounced press, so a rider who has just wired eight unlabelled switches
 * can press one and be told which input it is. It is off unless this screen
 * is showing it, and the device drops it on disconnect regardless.
 *
 * Everything here edits a local copy of the config and commits on Save, the
 * same model as OutputsScreen — one config write, not a write per tap.
 */
import React, {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { INPUT_COUNT, OUTPUT_COUNT } from '../protocol/constants';
import type { InputEvent, MotoClient } from '../protocol/MotoClient';
import {
  ACTION_HAZARD_TOGGLE,
  ACTION_LIST_MAX,
  ACTION_TURN_L_TOGGLE,
  ACTION_TURN_R_TOGGLE,
  actionAlternateOutput,
  actionToggleOutput,
  behavioursForTrigger,
  outputChannelForAction,
  outputChannelForAlternateAction,
  type ComboDef,
  type DeviceConfig,
  type OutputBehaviour,
} from '../protocol/types';
import {
  Badge,
  Button,
  Card,
  Chip,
  Divider,
  EmptyState,
  Field,
  SkeletonScreen,
  Notice,
  NumberField,
  Screen,
  SectionHeader,
  useLeaveGuard,
} from '../ui/components';
import { colors, radius, space, type } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

/** The three press types, in the order a rider thinks about them. */
const PRESS_ROWS = [
  { key: 'short_press_action', label: 'Single press', trigger: 'tap' },
  { key: 'double_press_action', label: 'Double press', trigger: 'double' },
  { key: 'long_press_action', label: 'Hold', trigger: 'hold' },
] as const;

type PressKey = (typeof PRESS_ROWS)[number]['key'];

/** The channel an action id ultimately drives, or null for hazards (which
 * drive a whole group rather than one channel). */
function channelForAction(id: number, config: DeviceConfig): number | null {
  const direct = outputChannelForAction(id);
  if (direct !== null) return direct;
  const side =
    id === ACTION_TURN_L_TOGGLE
      ? 'left'
      : id === ACTION_TURN_R_TOGGLE
        ? 'right'
        : null;
  if (side === null) return null;
  const idx = config.outputs.channels.findIndex(c => c.indicator === side);
  return idx >= 0 ? idx : null;
}

function behaviourOfAction(
  id: number,
  config: DeviceConfig,
): OutputBehaviour | null {
  const ch = channelForAction(id, config);
  const alt = outputChannelForAlternateAction(id);
  const index = ch ?? alt;
  return index === null
    ? null
    : (config.outputs.channels[index]?.behaviour ?? null);
}

/** Whether an action can sensibly be bound to a given trigger. Encodes the
 * product matrix (see behavioursForTrigger): a momentary output bound to a
 * single tap would simply never fire, because momentary follows a HOLD — so
 * the picker greys it out rather than letting a rider build a dead binding.
 * Hazards are always allowed: they drive a group, not one channel. */
function actionAllowedForTrigger(
  id: number,
  trigger: 'tap' | 'double' | 'hold' | 'chord',
  config: DeviceConfig,
): boolean {
  if (id === ACTION_HAZARD_TOGGLE) return true;
  const behaviour = behaviourOfAction(id, config);
  if (behaviour === null) return true;
  return behavioursForTrigger(trigger).includes(behaviour);
}

/** Which output/action a chip in the picker represents. Function-based ids
 * come first because they're the special-behaviour ones. */
function actionLabel(id: number, config: DeviceConfig): string {
  if (id === ACTION_TURN_L_TOGGLE) return 'Turn left';
  if (id === ACTION_TURN_R_TOGGLE) return 'Turn right';
  if (id === ACTION_HAZARD_TOGGLE) return 'Hazards';
  const ch = outputChannelForAction(id);
  if (ch !== null) return channelName(ch, config);
  /* A pair binding names both members, in the order a press visits them from
   * cold, so "Low Beam / High Beam" tells the rider which one the first tap
   * lights. */
  const alt = outputChannelForAlternateAction(id);
  if (alt !== null) {
    const partner = config.outputs.channels[alt]?.alternate_channel ?? -1;
    return partner >= 0
      ? `${channelName(alt, config)} / ${channelName(partner, config)}`
      : channelName(alt, config);
  }
  return `Action ${id}`;
}

function channelName(ch: number, config: DeviceConfig): string {
  const name = config.outputs.channels[ch]?.name;
  return name && name.trim() ? name : `Output ${ch + 1}`;
}

export function buttonLabel(index: number, config: DeviceConfig | null): string {
  const name = config?.inputs.names[index];
  return name && name.trim() ? name : `Button ${index + 1}`;
}

export function ButtonsScreen({ client, onDone }: Props): React.JSX.Element {
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);

  /* Identify mode. `lastPress` is what the rider just pressed; `expanded`
   * is which button's bindings are open. */
  const [identifying, setIdentifying] = useState(false);
  const [lastPress, setLastPress] = useState<InputEvent | null>(null);
  const [expanded, setExpanded] = useState<number | null>(null);
  const [editing, setEditing] = useState<{
    button: number;
    press: PressKey;
  } | null>(null);

  /** Serialised copy of what the board last gave us — see OutputsScreen. */
  const [baseline, setBaseline] = useState<string | null>(null);

  useEffect(() => {
    client
      .configRead()
      .then(c => {
        setConfig(c);
        setBaseline(JSON.stringify(c));
      })
      .catch((err: unknown) =>
        setError(err instanceof Error ? err.message : String(err)),
      )
      .finally(() => setLoading(false));
  }, [client]);

  const dirty = config !== null && JSON.stringify(config) !== baseline;
  const back = useLeaveGuard(dirty, onDone);

  /* Learn mode must not outlive this screen: if the rider navigates away (or
   * the component unmounts for any reason) turn it back off, so the board
   * isn't notifying on every press for the rest of the ride. The device also
   * clears it on disconnect, so a failed request here is not fatal — hence
   * the swallowed rejection. */
  const identifyingRef = useRef(false);
  useEffect(() => {
    identifyingRef.current = identifying;
  }, [identifying]);
  useEffect(() => {
    return () => {
      if (identifyingRef.current) {
        client.inputLearn(false).catch(() => {});
      }
    };
  }, [client]);

  useEffect(() => {
    if (!identifying) return undefined;
    return client.onInputEvent(event => {
      setLastPress(event);
      setExpanded(event.button);
    });
  }, [client, identifying]);

  const toggleIdentify = useCallback(async () => {
    const next = !identifying;
    setError(null);
    try {
      const result = await client.inputLearn(next);
      if (!result.ok) {
        setError(`Device refused identify mode: ${result.resultName}`);
        return;
      }
      setIdentifying(next);
      if (!next) setLastPress(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    }
  }, [client, identifying]);

  /* --- config edits (local until Save) --- */

  const setName = useCallback((index: number, name: string) => {
    setConfig(prev => {
      if (!prev) return prev;
      const names = prev.inputs.names.slice();
      names[index] = name;
      return { ...prev, inputs: { ...prev.inputs, names } };
    });
    setSaved(false);
  }, []);

  const toggleAction = useCallback(
    (button: number, press: PressKey, actionId: number) => {
      setConfig(prev => {
        if (!prev) return prev;
        const arrays = prev.inputs[press].map(a => a.slice());
        const current = arrays[button] ?? [];
        const at = current.indexOf(actionId);
        if (at >= 0) {
          current.splice(at, 1);
        } else {
          // Silently ignoring the tap past the cap would look broken; the UI
          // disables the chips instead (see canAdd below), so this is a guard.
          if (current.length >= ACTION_LIST_MAX) return prev;
          current.push(actionId);
        }
        arrays[button] = current;
        return { ...prev, inputs: { ...prev.inputs, [press]: arrays } };
      });
      setSaved(false);
    },
    [],
  );

  const addChord = useCallback(() => {
    setConfig(prev => {
      if (!prev) return prev;
      const combo: ComboDef = {
        type: 'chord',
        buttons: [],
        actions: [],
        window_ms: 120,
      };
      return {
        ...prev,
        inputs: { ...prev.inputs, combos: [...prev.inputs.combos, combo] },
      };
    });
    setSaved(false);
  }, []);

  const updateChord = useCallback((index: number, patch: Partial<ComboDef>) => {
    setConfig(prev => {
      if (!prev) return prev;
      const combos = prev.inputs.combos.slice();
      combos[index] = { ...combos[index]!, ...patch };
      return { ...prev, inputs: { ...prev.inputs, combos } };
    });
    setSaved(false);
  }, []);

  const removeChord = useCallback((index: number) => {
    setConfig(prev => {
      if (!prev) return prev;
      const combos = prev.inputs.combos.slice();
      combos.splice(index, 1);
      return { ...prev, inputs: { ...prev.inputs, combos } };
    });
    setSaved(false);
  }, []);

  /* Every action a binding chip can offer: the three flasher-behaviour ones,
   * one per output channel, then one per configured alternating pair.
   *
   * A pair contributes a single chip, keyed on its lower-numbered member,
   * not one per member: from cold a press lights whichever channel it names,
   * but after that the two are indistinguishable, so offering both would be
   * two chips that do the same thing on every press but the first. Depends
   * on config because pairs are configured on the Outputs screen. */
  const actionChoices = useMemo(() => {
    const ids = [
      ACTION_TURN_L_TOGGLE,
      ACTION_TURN_R_TOGGLE,
      ACTION_HAZARD_TOGGLE,
    ];
    for (let ch = 0; ch < OUTPUT_COUNT; ch++) ids.push(actionToggleOutput(ch));
    config?.outputs.channels.forEach((c, ch) => {
      if (c.alternate_channel > ch) ids.push(actionAlternateOutput(ch));
    });
    return ids;
  }, [config]);

  /* Buttons that appear in a chord: their own single-press bindings are
   * suppressed by the firmware when the chord fires, and the rider should
   * know that before wondering why nothing happened. */
  const chordMembers = useMemo(() => {
    const set = new Set<number>();
    for (const c of config?.inputs.combos ?? []) {
      if (c.type === 'chord') for (const b of c.buttons) set.add(b);
    }
    return set;
  }, [config]);

  async function save(): Promise<void> {
    if (!config) return;
    const emptyChord = config.inputs.combos.findIndex(
      c =>
        c.type === 'chord' && (c.buttons.length < 2 || c.actions.length === 0),
    );
    if (emptyChord >= 0) {
      setError(
        `Chord ${emptyChord + 1} needs at least two buttons and one output.`,
      );
      return;
    }
    setSaving(true);
    setError(null);
    setSaved(false);
    try {
      const result = await client.configWrite(config);
      if (result.ok) {
        setSaved(true);
        setBaseline(JSON.stringify(config));
      } else {
        setError(`Device rejected the config: ${result.resultName}`);
      }
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      setSaving(false);
    }
  }

  if (loading)
    return <SkeletonScreen title="Buttons" onBack={back} cards={4} lines={1} />;
  if (!config) {
    return (
      <Screen title="Buttons" onBack={back}>
        <Notice tone="danger">
          {error ?? 'Could not read the configuration from the device.'}
        </Notice>
      </Screen>
    );
  }

  return (
    <Screen
      title="Buttons"
      onBack={back}
      trailing={
        <Button
          label={saving ? 'Saving' : 'Save'}
          onPress={save}
          busy={saving}
          tone={dirty ? 'primary' : 'secondary'}
        />
      }
    >
      {error && <Notice tone="danger">{error}</Notice>}
      {saved && !dirty && <Notice tone="on">Saved to the device.</Notice>}

      {/* --- identify --- */}
      <Card>
        <View style={styles.rowBetween}>
          <View style={styles.flex}>
            <Text style={type.heading}>Identify a button</Text>
            <Text style={type.caption}>
              Turn this on, then press a switch on the handlebar. The board will
              tell you which input it is wired to.
            </Text>
          </View>
          <Button
            label={identifying ? 'Stop' : 'Start'}
            tone={identifying ? 'secondary' : 'primary'}
            onPress={toggleIdentify}
            accessibilityLabel={
              identifying ? 'Stop identify mode' : 'Start identify mode'
            }
          />
        </View>

        {identifying && (
          <View style={styles.identifyPanel}>
            {lastPress ? (
              <>
                <Text style={styles.identifyHeadline}>
                  {buttonLabel(lastPress.button, config)}
                </Text>
                <View style={styles.rowWrap}>
                  <Badge
                    label={`INPUT ${lastPress.button + 1}`}
                    tone="accent"
                  />
                  <Badge
                    label={lastPress.pressType.toUpperCase()}
                    tone="info"
                  />
                  {lastPress.actionSuppressed && (
                    <Badge label="CHORD TOOK IT" tone="warn" />
                  )}
                </View>
                <Field
                  label="Name this button"
                  value={config.inputs.names[lastPress.button] ?? ''}
                  onChangeText={v => setName(lastPress.button, v)}
                  placeholder={`Button ${lastPress.button + 1}`}
                  maxLength={23}
                />
              </>
            ) : (
              <Text style={styles.identifyWaiting}>Waiting for a press…</Text>
            )}
          </View>
        )}
      </Card>

      {/* --- per-button bindings --- */}
      <SectionHeader hint="Tap a button to bind its presses to outputs.">
        Handlebar buttons
      </SectionHeader>

      {Array.from({ length: INPUT_COUNT }).map((_, index) => {
        const isOpen = expanded === index;
        const bound = PRESS_ROWS.reduce(
          (n, r) => n + (config.inputs[r.key][index]?.length ?? 0),
          0,
        );
        return (
          <Card key={index} padded={false}>
            <Pressable
              onPress={() => {
                setExpanded(isOpen ? null : index);
                setEditing(null);
              }}
              accessibilityRole="button"
              accessibilityState={{ expanded: isOpen }}
              accessibilityLabel={`${buttonLabel(index, config)}, ${bound} binding${bound === 1 ? '' : 's'}`}
              style={({ pressed }) => [
                styles.buttonHeader,
                pressed && styles.buttonHeaderPressed,
              ]}
            >
              <View style={styles.inputPip}>
                <Text style={styles.inputPipText}>{index + 1}</Text>
              </View>
              <View style={styles.flex}>
                <Text style={type.heading} numberOfLines={1}>
                  {buttonLabel(index, config)}
                </Text>
                <Text style={type.caption}>
                  {bound === 0
                    ? 'Not bound'
                    : `${bound} binding${bound === 1 ? '' : 's'}`}
                  {chordMembers.has(index) ? ' · in a chord' : ''}
                </Text>
              </View>
              <Text style={styles.chevron}>{isOpen ? '⌄' : '›'}</Text>
            </Pressable>

            {isOpen && (
              <View style={styles.buttonBody}>
                <Divider />
                <Field
                  label="Name"
                  value={config.inputs.names[index] ?? ''}
                  onChangeText={v => setName(index, v)}
                  placeholder={`Button ${index + 1}`}
                  maxLength={23}
                />
                {chordMembers.has(index) && (
                  <Notice tone="info">
                    This button is part of a chord. When the chord fires, these
                    individual bindings are skipped on purpose — pressing it
                    alone still works normally.
                  </Notice>
                )}

                {PRESS_ROWS.map(row => {
                  const list = config.inputs[row.key][index] ?? [];
                  const isEditing =
                    editing?.button === index && editing.press === row.key;
                  const canAdd = list.length < ACTION_LIST_MAX;
                  return (
                    <View key={row.key} style={styles.pressRow}>
                      <View style={styles.rowBetween}>
                        <Text style={styles.pressLabel}>{row.label}</Text>
                        <Chip
                          label={
                            isEditing
                              ? 'Done'
                              : list.length
                                ? 'Change'
                                : 'Add output'
                          }
                          active={isEditing}
                          onPress={() =>
                            setEditing(
                              isEditing
                                ? null
                                : { button: index, press: row.key },
                            )
                          }
                          accessibilityLabel={`${isEditing ? 'Finish editing' : 'Edit'} ${row.label} for ${buttonLabel(index, config)}`}
                        />
                      </View>

                      {list.length > 0 && (
                        <View style={styles.rowWrap}>
                          {list.map(id => (
                            <Chip
                              key={id}
                              label={actionLabel(id, config)}
                              active
                              tone="on"
                            />
                          ))}
                        </View>
                      )}
                      {list.length === 0 && !isEditing && (
                        <Text style={type.caption}>Nothing bound</Text>
                      )}

                      {isEditing && (
                        <View style={styles.picker}>
                          <Text style={type.caption}>
                            {`Up to ${ACTION_LIST_MAX} outputs. ` +
                              (row.trigger === 'hold'
                                ? 'A hold can drive momentary or toggle outputs.'
                                : 'A tap can drive toggle or blink outputs — momentary ones need a hold.')}
                          </Text>
                          <View style={styles.rowWrap}>
                            {actionChoices.map(id => {
                              const selected = list.includes(id);
                              const allowed = actionAllowedForTrigger(
                                id,
                                row.trigger,
                                config,
                              );
                              return (
                                <Chip
                                  key={id}
                                  label={actionLabel(id, config)}
                                  active={selected}
                                  tone={selected ? 'on' : 'neutral'}
                                  disabled={!allowed || (!selected && !canAdd)}
                                  onPress={() =>
                                    toggleAction(index, row.key, id)
                                  }
                                />
                              );
                            })}
                          </View>
                        </View>
                      )}
                    </View>
                  );
                })}
              </View>
            )}
          </Card>
        );
      })}

      {/* --- chords --- */}
      <SectionHeader hint="Several buttons pressed together, driving a set of outputs. Classic use: both turn-signal buttons at once for hazards.">
        Chords
      </SectionHeader>

      {config.inputs.combos.length === 0 && (
        <Card>
          <EmptyState
            title="No chords yet"
            body="Add one to bind a combination of buttons to an action."
          />
        </Card>
      )}

      {config.inputs.combos.map((combo, ci) => {
        if (combo.type !== 'chord') {
          /* Sequence combos are the unlock cheat-code mechanism, edited on the
           * Lock screen — showing them here as if they were chords would
           * invite someone to break their own immobilizer. */
          return (
            <Card key={ci}>
              <View style={styles.rowBetween}>
                <Text style={type.bodyMuted}>
                  Sequence combo — edited in Lock settings
                </Text>
                <Badge label="SEQUENCE" />
              </View>
            </Card>
          );
        }
        return (
          <Card key={ci}>
            <View style={styles.rowBetween}>
              <Text style={type.heading}>Chord {ci + 1}</Text>
              <Chip
                label="Remove"
                tone="danger"
                onPress={() => removeChord(ci)}
                accessibilityLabel={`Remove chord ${ci + 1}`}
              />
            </View>

            <Text style={styles.overlineTight}>Buttons pressed together</Text>
            <View style={styles.rowWrap}>
              {Array.from({ length: INPUT_COUNT }).map((_, b) => {
                const selected = combo.buttons.includes(b);
                return (
                  <Chip
                    key={b}
                    label={buttonLabel(b, config)}
                    active={selected}
                    onPress={() =>
                      updateChord(ci, {
                        buttons: selected
                          ? combo.buttons.filter(x => x !== b)
                          : [...combo.buttons, b].sort(),
                      })
                    }
                  />
                );
              })}
            </View>

            <Text style={styles.overlineTight}>Outputs</Text>
            <View style={styles.rowWrap}>
              {actionChoices.map(id => {
                const selected = combo.actions.includes(id);
                const canAdd = combo.actions.length < ACTION_LIST_MAX;
                /* A chord fires on press-down (an edge) but is also held, so
                 * both the tap set and momentary are legitimate here. */
                const allowed =
                  actionAllowedForTrigger(id, 'chord', config) ||
                  actionAllowedForTrigger(id, 'hold', config);
                return (
                  <Chip
                    key={id}
                    label={actionLabel(id, config)}
                    active={selected}
                    tone={selected ? 'on' : 'neutral'}
                    disabled={!allowed || (!selected && !canAdd)}
                    onPress={() =>
                      updateChord(ci, {
                        actions: selected
                          ? combo.actions.filter(x => x !== id)
                          : [...combo.actions, id],
                      })
                    }
                  />
                );
              })}
            </View>

            <NumberField
              label="Press window (ms)"
              value={combo.window_ms}
              min={0}
              onChangeValue={v => updateChord(ci, { window_ms: v })}
              hint="How far apart the presses may be and still count as simultaneous. 120ms suits most riders."
            />
          </Card>
        );
      })}

      <Button label="Add chord" tone="secondary" onPress={addChord} />

      <Notice tone="info">
        Bindings are applied with the same safety rules as app commands: a
        locked immobilizer still blocks ignition, and the starter still refuses
        to fire while the engine is running or the interlock is open.
      </Notice>
    </Screen>
  );
}

const styles = StyleSheet.create({
  flex: { flex: 1 },
  rowBetween: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
  },
  rowWrap: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },

  identifyPanel: {
    marginTop: space.sm,
    padding: space.md,
    borderRadius: radius.md,
    backgroundColor: colors.sunken,
    borderWidth: 1,
    borderColor: colors.accentBorder,
    gap: space.sm,
  },
  identifyHeadline: { fontSize: 20, fontWeight: '700', color: colors.accent },
  identifyWaiting: {
    ...type.bodyMuted,
    textAlign: 'center',
    paddingVertical: space.md,
  },

  /* Whole row is the touch target. Height comes from the content, so a long
   * button name or a larger accessibility font grows the row instead of
   * clipping. Nested pressables live only in the expanded body below, never
   * inside this one. */
  buttonHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    minHeight: 64,
    paddingVertical: space.md,
    paddingHorizontal: space.md,
    gap: space.md,
  },
  buttonHeaderPressed: {
    backgroundColor: colors.raisedHover,
    borderRadius: radius.lg,
  },
  inputPip: {
    width: 32,
    height: 32,
    borderRadius: radius.sm,
    backgroundColor: colors.raisedHover,
    alignItems: 'center',
    justifyContent: 'center',
  },
  inputPipText: {
    ...type.valueSmall,
    color: colors.textMuted,
    fontWeight: '700',
  },
  chevron: { fontSize: 20, color: colors.textFaint },
  buttonBody: {
    paddingHorizontal: space.md,
    paddingBottom: space.md,
    gap: space.md,
  },

  pressRow: { gap: space.sm },
  pressLabel: { ...type.body, fontWeight: '600' },
  overlineTight: { ...type.overline, marginTop: space.xs },
  picker: {
    padding: space.md,
    borderRadius: radius.md,
    backgroundColor: colors.sunken,
    borderWidth: 1,
    borderColor: colors.border,
    gap: space.sm,
  },
});
