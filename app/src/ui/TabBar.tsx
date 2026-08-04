/**
 * Bottom tab bar.
 *
 * Replaces the grid of navigation tiles that used to sit under the dashboard.
 * That grid grew one tile per screen and had reached nine — by then it was a
 * menu pretending to be a dashboard, and the live control a rider actually
 * opens the app for was being pushed off the bottom of the scroll.
 *
 * Text only, no icons. An icon set is either a native dependency or a pile of
 * hand-drawn Views, and four words at 13pt are easier to read through a visor
 * than four 20pt pictograms whose meaning has to be learned. The active tab
 * carries the accent colour and a rule above it, so state reads without
 * relying on colour alone.
 *
 * On iOS 26 the bar is real Liquid Glass (`@callstack/liquid-glass`, MIT —
 * the effect is UIKit's, not a gradient impersonating it). Everywhere else —
 * Android, and iOS before 26 — it falls back to an opaque raised surface with
 * a hairline top border. The fallback is deliberately opaque rather than a
 * translucent approximation: a half-transparent bar over scrolling content
 * with no blur behind it is harder to read than a solid one.
 *
 * The bar OVERLAYS the content rather than sitting in the layout flow below
 * it. That is what makes the glass visible at all: the effect refracts
 * whatever is behind it, so a bar with only the flat page background behind
 * it renders as a flat page background. Callers must therefore inset their
 * scroll content by `useTabBarHeight()` so nothing ends up permanently
 * trapped underneath.
 */
import React from 'react';
import { Platform, Pressable, StyleSheet, Text, View } from 'react-native';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import {
  isLiquidGlassSupported,
  LiquidGlassView,
} from '@callstack/liquid-glass';

import { colors, MIN_TOUCH, space, type } from './theme';

/** Bar height above the home-indicator inset. */
const BAR_CONTENT_HEIGHT = MIN_TOUCH + space.sm * 2;

/** Total height the bar occupies, for insetting content that scrolls under
 * it. Follows the safe area, so it is a hook rather than a constant. */
export function useTabBarHeight(): number {
  const insets = useSafeAreaInsets();
  return BAR_CONTENT_HEIGHT + Math.max(insets.bottom, space.sm);
}

export interface TabDef<T extends string> {
  key: T;
  label: string;
}

interface Props<T extends string> {
  tabs: readonly TabDef<T>[];
  active: T;
  onSelect: (key: T) => void;
}

export function TabBar<T extends string>({
  tabs,
  active,
  onSelect,
}: Props<T>): React.JSX.Element {
  const insets = useSafeAreaInsets();
  const glass = Platform.OS === 'ios' && isLiquidGlassSupported;

  /* The home-indicator inset is padding INSIDE the bar, not a gap under it —
   * the material has to run to the bottom of the screen or it reads as a
   * floating slab with a strip of content showing beneath. */
  const pad = { paddingBottom: Math.max(insets.bottom, space.sm) };

  const items = (
    <View style={styles.row}>
      {tabs.map(tab => {
        const selected = tab.key === active;
        return (
          <Pressable
            key={tab.key}
            onPress={() => onSelect(tab.key)}
            accessibilityRole="tab"
            accessibilityState={{ selected }}
            accessibilityLabel={tab.label}
            style={({ pressed }) => [styles.tab, pressed && styles.tabPressed]}
          >
            <View
              style={[styles.rule, selected && styles.ruleActive]}
              accessibilityElementsHidden
              importantForAccessibility="no"
            />
            <Text
              style={[styles.label, selected && styles.labelActive]}
              numberOfLines={1}
            >
              {tab.label}
            </Text>
          </Pressable>
        );
      })}
    </View>
  );

  if (glass) {
    return (
      <LiquidGlassView
        effect="regular"
        colorScheme="dark"
        style={[styles.bar, pad]}
      >
        {items}
      </LiquidGlassView>
    );
  }

  return <View style={[styles.bar, styles.barOpaque, pad]}>{items}</View>;
}

const styles = StyleSheet.create({
  bar: {
    position: 'absolute',
    left: 0,
    right: 0,
    bottom: 0,
    paddingTop: space.sm,
  },
  barOpaque: {
    backgroundColor: colors.raised,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: colors.borderStrong,
  },
  row: { flexDirection: 'row', alignItems: 'stretch' },
  tab: {
    flex: 1,
    minHeight: MIN_TOUCH,
    alignItems: 'center',
    justifyContent: 'center',
    gap: space.xs,
    paddingHorizontal: space.xs,
  },
  tabPressed: { opacity: 0.6 },
  /* Above the label rather than under it: a rule at the very bottom of the
   * screen collides with the home indicator on iOS. */
  rule: {
    height: 2,
    width: 20,
    borderRadius: 1,
    backgroundColor: 'transparent',
  },
  ruleActive: { backgroundColor: colors.accent },
  label: {
    ...type.caption,
    fontSize: 13,
    fontWeight: '600',
    color: colors.textMuted,
  },
  labelActive: { color: colors.accent },
});
