/**
 * Settings: the one hub screen behind the Ride screen's round settings
 * button. Used to be three separate tabs (Setup, Security, Settings) — with
 * the bottom tab bar gone in favour of Ride-as-landing-screen, three peer
 * hubs one tap apart no longer made sense as three *tabs*, but their
 * contents are still just lists of rows, so they became named sections of
 * this one list instead of merging into an undifferentiated pile. Nothing
 * here lost a destination; each row is a `SectionHeader` + `Card` group
 * instead of its own screen.
 *
 * Two sections, not the original three: Immobilizer and Paired keys read as
 * configuration (they're both "how this bike behaves"), not a separate
 * category of their own, so they moved into Configuration alongside Outputs
 * and Buttons rather than keeping their own Security heading. Board moved
 * the other way, into Device, next to the other read-the-hardware-state
 * items (Diagnostics, Firmware, Event log) rather than sitting with the
 * behavioural config.
 *
 * Disconnect lives here now too (out of the Ride screen's title bar, which
 * only needed room for the settings button), Board info stays as the
 * header above the sections, same as it was on the old Settings tab.
 */
import React from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { Button, Card, Divider, Screen, SectionHeader } from '../ui/components';
import { colors, MIN_TOUCH, space, type } from '../ui/theme';

export interface SectionItem {
  label: string;
  detail: string;
  onPress: () => void;
}

function Section({
  title,
  items,
}: {
  title: string;
  items: readonly SectionItem[];
}): React.JSX.Element {
  return (
    <>
      <SectionHeader>{title}</SectionHeader>
      <Card padded={false}>
        {items.map((item, i) => (
          <View key={item.label}>
            {i > 0 && <Divider />}
            <Pressable
              onPress={item.onPress}
              accessibilityRole="button"
              accessibilityLabel={`${item.label}. ${item.detail}`}
              style={({ pressed }) => [styles.row, pressed && styles.pressed]}
            >
              <View style={styles.text}>
                <Text style={type.heading}>{item.label}</Text>
                <Text style={type.caption}>{item.detail}</Text>
              </View>
              <Text style={styles.chevron}>›</Text>
            </Pressable>
          </View>
        ))}
      </Card>
    </>
  );
}

interface Props {
  /** Board info card, rendered above every section — same spot the old
   * Settings tab put it. */
  header?: React.ReactNode;
  configurationItems: readonly SectionItem[];
  deviceItems: readonly SectionItem[];
  onBack: () => void;
  onDisconnect: () => void;
}

export function SettingsScreen({
  header,
  configurationItems,
  deviceItems,
  onBack,
  onDisconnect,
}: Props): React.JSX.Element {
  return (
    <Screen
      title="Settings"
      onBack={onBack}
      trailing={<Button label="Disconnect" tone="ghost" onPress={onDisconnect} />}
    >
      {header}
      <Section title="Configuration" items={configurationItems} />
      <Section title="Device" items={deviceItems} />
    </Screen>
  );
}

const styles = StyleSheet.create({
  row: {
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    minHeight: MIN_TOUCH + space.md,
    paddingHorizontal: space.md,
    paddingVertical: space.md,
  },
  pressed: { backgroundColor: colors.raisedHover },
  text: { flex: 1, gap: 2 },
  chevron: { fontSize: 20, color: colors.textFaint },
});
