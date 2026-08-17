/**
 * Settings: the one hub screen behind the Ride screen's settings button.
 * Every destination other than Ride and Pairing is reached from here, as a
 * list of `SectionHeader` + `Card` groups, each row pushing one screen deep.
 *
 * Two sections. Configuration is "how this bike behaves" — Outputs, Buttons,
 * Immobilizer, Paired keys. Device is "what the hardware is doing right
 * now" — Board, Diagnostics, Firmware, Event log. Board info (version,
 * uptime) is the header above both, and Disconnect sits at the bottom.
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
