/**
 * A tab's landing screen: the list of screens that tab holds.
 *
 * Rows rather than the grid of tiles this replaces. A grid made sense at four
 * destinations and stopped making sense at nine — two columns of truncated
 * two-line tiles are harder to scan than one column of full-width rows, and a
 * row gives the description room to be a sentence instead of an abbreviation.
 */
import React from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';

import { Card, Divider, Screen } from '../ui/components';
import { colors, MIN_TOUCH, space, type } from '../ui/theme';

export interface SectionItem {
  label: string;
  detail: string;
  onPress: () => void;
}

interface Props {
  title: string;
  items: readonly SectionItem[];
  /** Rendered above the list — board info on Settings, for instance. */
  header?: React.ReactNode;
}

export function SectionScreen({
  title,
  items,
  header,
}: Props): React.JSX.Element {
  return (
    <Screen title={title}>
      {header}
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
