/**
 * Outputs: name each of the 12 channels, pick what it does, and mark the few
 * roles that carry real behaviour.
 *
 * A free-text name, one behaviour dropdown, and explicit role switches.
 * Naming and behaviour are kept as separate questions, and only roles that
 * actually change firmware behaviour are offered, each saying what it does.
 *
 * Roles are switches rather than implied by a name on purpose: a safety
 * property must never hide inside a label, where a channel would be
 * protected from the low-voltage cutoff only if the rider happened to call
 * it a headlight.
 */
import React, { useEffect, useMemo, useState } from 'react';
import { Pressable, StyleSheet, Switch, Text, View } from 'react-native';

import { INPUT_COUNT, LOCK_STATE } from '../protocol/constants';
import type { MotoClient } from '../protocol/MotoClient';
import {
  BEHAVIOUR_HINTS,
  BEHAVIOUR_LABELS,
  INDICATOR_LABELS,
  INDICATOR_SIDES,
  OUTPUT_BEHAVIOURS,
  type DeviceConfig,
  type IndicatorSide,
  type OutputBehaviour,
  isOutputOn,
  type OutputChannelConfig,
  type Status,
} from '../protocol/types';
import {
  Badge,
  Button,
  Card,
  Field,
  SkeletonScreen,
  Notice,
  NumberField,
  Screen,
  SectionHeader,
  Select,
  ToggleRow,
  useLeaveGuard,
} from '../ui/components';
import { colors, space, type } from '../ui/theme';

interface Props {
  client: MotoClient;
  onDone: () => void;
}

const BEHAVIOUR_OPTIONS = OUTPUT_BEHAVIOURS.map(b => ({
  value: b,
  label: BEHAVIOUR_LABELS[b],
  hint: BEHAVIOUR_HINTS[b],
}));

const INDICATOR_OPTIONS = INDICATOR_SIDES.map(s => ({
  value: s,
  label: INDICATOR_LABELS[s],
}));

const INPUT_OPTIONS = [
  { value: '-1', label: 'None' },
  ...Array.from({ length: INPUT_COUNT }, (_, i) => ({
    value: String(i),
    label: `Button ${i + 1}`,
  })),
];

function channelLabel(ch: OutputChannelConfig, index: number): string {
  return ch.name.trim() || `Output ${index + 1}`;
}

export function OutputsScreen({ client, onDone }: Props): React.JSX.Element {
  const [config, setConfig] = useState<DeviceConfig | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [expanded, setExpanded] = useState<number | null>(null);
  /** What the board last told us, so leaving can tell edits from no edits.
   * Serialised rather than kept as an object: the whole config is replaced on
   * every keystroke, so reference equality says nothing. */
  const [baseline, setBaseline] = useState<string | null>(null);
  /* Live switching lives here now rather than on the Ride screen. Twelve
   * switches were the bulk of a screen a rider opens with the engine warm,
   * and the channel's name and behaviour — the things that tell you what a
   * switch will do — were on this screen anyway. */
  const [status, setStatus] = useState<Status | null>(client.getLastStatus());
  const [pending, setPending] = useState<Set<number>>(new Set());

  useEffect(() => client.onStatus(setStatus), [client]);

  async function toggle(channel: number, on: boolean): Promise<void> {
    setPending(prev => new Set(prev).add(channel));
    try {
      await client.setOutput(channel, on);
    } catch {
      /* The status stream reports true device state regardless. */
    } finally {
      setPending(prev => {
        const next = new Set(prev);
        next.delete(channel);
        return next;
      });
    }
  }

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

  function patchChannel(ch: number, patch: Partial<OutputChannelConfig>): void {
    setConfig(prev => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.slice();
      channels[ch] = { ...channels[ch]!, ...patch };
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
    setSaved(false);
  }

  /* Ignition and starter are at-most-one, and the firmware rejects a config
   * with two. Clearing the flag on every other channel keeps the UI from
   * letting you build something that will only fail on save. */
  function setExclusiveRole(
    ch: number,
    role: 'is_ignition' | 'is_starter',
    on: boolean,
  ): void {
    setConfig(prev => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.map((c, i) =>
        i === ch ? { ...c, [role]: on } : on ? { ...c, [role]: false } : c,
      );
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
    setSaved(false);
  }

  /* Pairing is reciprocal, and the firmware rejects a config where it isn't
   * (MC_OUT_CFG_BAD_ALTERNATE) — a one-way link would mean switching A on
   * cancels B, but switching B on leaves A lit, i.e. both beams burning.
   * Rather than surface that as a save-time rejection, both sides are always
   * written together here, and any partner either channel is being taken
   * away from is released in the same pass. */
  function setAlternate(ch: number, partner: number): void {
    setConfig(prev => {
      if (!prev) return prev;
      const channels = prev.outputs.channels.map(c => ({ ...c }));
      const oldPartner = channels[ch]!.alternate_channel;
      if (oldPartner >= 0 && channels[oldPartner])
        channels[oldPartner]!.alternate_channel = -1;
      if (partner >= 0) {
        const partnersOld = channels[partner]!.alternate_channel;
        if (partnersOld >= 0 && channels[partnersOld])
          channels[partnersOld]!.alternate_channel = -1;
        channels[partner]!.alternate_channel = ch;
      }
      channels[ch]!.alternate_channel = partner;
      return { ...prev, outputs: { ...prev.outputs, channels } };
    });
    setSaved(false);
  }

  function patchOutputs(patch: Partial<DeviceConfig['outputs']>): void {
    setConfig(prev =>
      prev ? { ...prev, outputs: { ...prev.outputs, ...patch } } : prev,
    );
    setSaved(false);
  }

  /* Offers "None", whichever channel is already this one's partner, and any
   * channel that is currently unpaired. A channel paired to someone else is
   * left out rather than shown and silently stealing that partner — the
   * reciprocal write would break a pair the rider set up elsewhere on this
   * screen with no indication it had happened. */
  function alternateOptionsFor(ch: number): { value: string; label: string }[] {
    const channels = config?.outputs.channels ?? [];
    const options = [{ value: '-1', label: 'None' }];
    channels.forEach((c, i) => {
      if (i === ch) return;
      const free = c.alternate_channel < 0 || c.alternate_channel === ch;
      if (free) options.push({ value: String(i), label: channelLabel(c, i) });
    });
    return options;
  }

  const hazardCount = useMemo(
    () => config?.outputs.channels.filter(c => c.hazard_member).length ?? 0,
    [config],
  );

  async function save(): Promise<void> {
    if (!config) return;
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
    return <SkeletonScreen title="Outputs" onBack={back} cards={5} lines={1} />;
  if (!config) {
    return (
      <Screen title="Outputs" onBack={back}>
        <Notice tone="danger">
          {error ?? 'Could not read the configuration from the device.'}
        </Notice>
      </Screen>
    );
  }

  const isOn = (ch: number): boolean =>
    status ? isOutputOn(status, ch) : false;
  const faultOn = (ch: number): boolean =>
    status ? (status.outputFaultMask & (1 << ch)) !== 0 : false;
  /* A locked bike switches nothing on, so the switches are
   * inert rather than offering commands the board will refuse. */
  const locked = status?.lockState === LOCK_STATE.LOCKED;

  return (
    <Screen
      title="Outputs"
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

      <SectionHeader hint="Name each channel and choose what it does. Tap one to open it.">
        Channels
      </SectionHeader>

      {config.outputs.channels.map((ch, i) => {
        const isOpen = expanded === i;
        const roles: string[] = [];
        if (ch.is_ignition) roles.push('IGNITION');
        if (ch.is_starter) roles.push('STARTER');
        if (ch.is_brake) roles.push('BRAKE');
        if (ch.indicator !== 'none') roles.push(ch.indicator.toUpperCase());
        if (ch.hazard_member) roles.push('HAZARD');
        if (ch.on_with_ignition) roles.push('WITH IGNITION');
        if (ch.alternate_channel >= 0) roles.push('PAIRED');
        if (ch.essential) roles.push('ESSENTIAL');

        return (
          <Card key={i} padded={false}>
            <Pressable
              onPress={() => setExpanded(isOpen ? null : i)}
              accessibilityRole="button"
              accessibilityState={{ expanded: isOpen }}
              accessibilityLabel={`${channelLabel(ch, i)}, ${BEHAVIOUR_LABELS[ch.behaviour]}`}
              style={({ pressed }) => [
                styles.headerRow,
                pressed && styles.headerRowPressed,
              ]}
            >
              <View style={styles.pip}>
                <Text style={styles.pipText}>{i + 1}</Text>
              </View>
              <View style={styles.headerText}>
                <Text style={type.heading} numberOfLines={1}>
                  {channelLabel(ch, i)}
                </Text>
                <Text style={type.caption} numberOfLines={1}>
                  {BEHAVIOUR_LABELS[ch.behaviour]}
                  {ch.pwm_duty_pct < 100 ? ` · dimmed ${ch.pwm_duty_pct}%` : ''}
                </Text>
              </View>
              <Text style={styles.chevron}>{isOpen ? '⌄' : '›'}</Text>
            </Pressable>

            {/* Outside the Pressable: a switch nested inside a row that is
             * itself a button would fight it for the touch. */}
            <View style={styles.liveRow}>
              <Text style={type.caption}>
                {ch.is_starter
                  ? 'Button only — never switchable from the app'
                  : locked
                    ? 'Locked — unlock the bike to switch outputs'
                    : faultOn(i)
                      ? 'Reporting a fault'
                      : isOn(i)
                        ? 'On'
                        : 'Off'}
              </Text>
              <Switch
                value={isOn(i)}
                disabled={ch.is_starter || locked || pending.has(i) || dirty}
                onValueChange={v => void toggle(i, v)}
                trackColor={{ false: colors.borderStrong, true: colors.on }}
                thumbColor={colors.text}
                ios_backgroundColor={colors.borderStrong}
                accessibilityLabel={`${channelLabel(ch, i)}, ${isOn(i) ? 'on' : 'off'}`}
              />
            </View>

            {roles.length > 0 && (
              <View style={styles.badgeRow}>
                {roles.map(r => (
                  <Badge
                    key={r}
                    label={r}
                    tone={
                      r === 'STARTER' || r === 'IGNITION' ? 'warn' : 'neutral'
                    }
                  />
                ))}
              </View>
            )}

            {isOpen && (
              <View style={styles.body}>
                <Field
                  label="Name"
                  value={ch.name}
                  onChangeText={v => patchChannel(i, { name: v })}
                  placeholder={`Output ${i + 1}`}
                  maxLength={23}
                />

                <Select<OutputBehaviour>
                  label="Behaviour"
                  value={ch.behaviour}
                  options={BEHAVIOUR_OPTIONS}
                  onChange={v => patchChannel(i, { behaviour: v })}
                />

                {(ch.behaviour === 'toggle' ||
                  ch.behaviour === 'momentary') && (
                  <NumberField
                    label="Brightness (%)"
                    value={ch.pwm_duty_pct}
                    min={1}
                    max={100}
                    onChangeValue={v => patchChannel(i, { pwm_duty_pct: v })}
                    hint="100 is full brightness. Dimming is off by default — some LED lamps flicker under PWM."
                  />
                )}

                <Select<IndicatorSide>
                  label="Indicator"
                  value={ch.indicator}
                  options={INDICATOR_OPTIONS}
                  onChange={v => patchChannel(i, { indicator: v })}
                  hint="Only indicators get turn auto-cancel and left/right mutual exclusion."
                />

                <ToggleRow
                  label="Blinks with hazards"
                  hint="Flashes with the indicators during a hazard stop, whatever this channel normally does. A steady light stays steady in normal use."
                  value={ch.hazard_member}
                  onValueChange={v => patchChannel(i, { hazard_member: v })}
                />

                <Select
                  label="Alternates with"
                  value={String(ch.alternate_channel)}
                  options={alternateOptionsFor(i)}
                  onChange={v => setAlternate(i, parseInt(v, 10))}
                  hint="Hi/lo beam, or two DRL colours. Only one of the pair is ever lit — switching one on puts the other out."
                />

                <ToggleRow
                  label="Comes on with ignition"
                  hint="Switches on when the ignition does and off when it does, like a key turned to “on”. You can still switch it off by hand; it stays off until the next ignition cycle."
                  value={ch.on_with_ignition}
                  disabled={ch.is_ignition}
                  onValueChange={v => patchChannel(i, { on_with_ignition: v })}
                />

                <ToggleRow
                  label="Essential"
                  hint="Never switched off by battery protection. Use for anything that must not go dark mid-ride."
                  value={ch.essential || ch.is_ignition || ch.is_brake}
                  disabled={ch.is_ignition || ch.is_brake}
                  onValueChange={v => patchChannel(i, { essential: v })}
                />

                <ToggleRow
                  label="Starter"
                  hint="Never switchable from the app. Blocked while the engine runs, and dropped the moment it starts."
                  value={ch.is_starter}
                  onValueChange={v => setExclusiveRole(i, 'is_starter', v)}
                />

                <ToggleRow
                  label="Ignition"
                  hint="The immobilizer's target, and how the bike knows it's running."
                  value={ch.is_ignition}
                  onValueChange={v => setExclusiveRole(i, 'is_ignition', v)}
                />

                <ToggleRow
                  label="Brake light"
                  hint="Driven directly by the brake switch below."
                  value={ch.is_brake}
                  onValueChange={v => patchChannel(i, { is_brake: v })}
                />
              </View>
            )}
          </Card>
        );
      })}

      <SectionHeader hint="Maintained switches wired straight through, rather than bound as button presses.">
        Existing switches
      </SectionHeader>
      <Card>
        <Select
          label="Brake switch"
          value={String(config.outputs.brake_switch_input)}
          options={INPUT_OPTIONS}
          onChange={v => patchOutputs({ brake_switch_input: parseInt(v, 10) })}
          hint="Drives the brake-light channel from the lever/pedal switch level."
        />
        <Select
          label="Starter interlock"
          value={String(config.outputs.starter_interlock_input)}
          options={INPUT_OPTIONS}
          onChange={v =>
            patchOutputs({ starter_interlock_input: parseInt(v, 10) })
          }
          hint="Neutral or clutch switch. When set, the starter won't fire unless it's engaged."
        />
      </Card>

      <SectionHeader>Timing</SectionHeader>
      <Card>
        {/* Seconds, not milliseconds: this is tens of seconds in practice,
          * and "30" reads as half a minute where "30000" reads as nothing.
          * Stored as ms either way — see NumberField's `scale`. */}
        <NumberField
          label="Turn auto-cancel (seconds)"
          value={config.outputs.turn_auto_cancel_ms}
          min={0}
          scale={1000}
          onChangeValue={v => patchOutputs({ turn_auto_cancel_ms: v })}
          hint="0 never auto-cancels."
        />
        <NumberField
          /* Stays in milliseconds: a blink period is a few hundred, and
           * "0.7" would be a worse way to say 700. */
          label="Blink period (ms)"
          value={config.outputs.turn_flash_period_ms}
          min={1}
          onChangeValue={v => patchOutputs({ turn_flash_period_ms: v })}
          hint="One full on+off cycle, for every channel set to blink."
        />
        <NumberField
          label="Flasher pulses"
          value={config.outputs.brake_flash_pulse_count}
          min={0}
          onChangeValue={v => patchOutputs({ brake_flash_pulse_count: v })}
          hint="Attention pulses before a flasher channel goes solid. 0 for none."
        />
      </Card>

      {hazardCount > 0 && hazardCount < 2 && (
        <Notice tone="warn">
          Only one channel blinks with the hazards. Most bikes want at least
          both indicators in the group.
        </Notice>
      )}
      <Notice tone="info">
        Brake flash patterns are not legal everywhere, which is why the flasher
        behaviour is opt-in per channel rather than the default for a brake
        light.
      </Notice>
    </Screen>
  );
}

const styles = StyleSheet.create({
  /* Whole row is the target, so height follows the content and a long name or
   * a larger accessibility font grows the row instead of clipping. Nested
   * pressables live only in the expanded body, never inside this. */
  headerRow: {
    flexDirection: 'row',
    alignItems: 'center',
    minHeight: 64,
    paddingHorizontal: space.md,
    paddingVertical: space.md,
    gap: space.md,
  },
  headerRowPressed: { backgroundColor: colors.raisedHover },
  liveRow: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    gap: space.md,
    paddingHorizontal: space.md,
    paddingBottom: space.sm,
  },
  headerText: { flex: 1 },
  pip: {
    width: 32,
    height: 32,
    borderRadius: 6,
    backgroundColor: colors.raisedHover,
    alignItems: 'center',
    justifyContent: 'center',
  },
  pipText: { ...type.valueSmall, fontWeight: '700' },
  chevron: { fontSize: 20, color: colors.textFaint },
  badgeRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: space.xs,
    paddingHorizontal: space.md,
    paddingBottom: space.sm,
  },
  body: {
    paddingHorizontal: space.md,
    paddingBottom: space.md,
    gap: space.md,
  },
});
