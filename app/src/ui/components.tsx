/**
 * Shared UI primitives, built on the tokens in theme.ts.
 *
 * Deliberately plain React Native — no UI library. Every dependency in this
 * app has to be justified against AGENTS.md's native-linking constraint (see
 * BlePlxTransport's header), and a component kit is a lot of surface area for
 * a dozen screens. These are small enough to read in one sitting.
 *
 * Anything a rider taps meets MIN_TOUCH, and every interactive element takes
 * an `accessibilityLabel`, because this app gets used with gloves on and with
 * a screen reader.
 */
import React from 'react';
import {
  ActivityIndicator,
  Modal,
  Pressable,
  ScrollView,
  StyleSheet,
  Switch,
  Text,
  TextInput,
  type TextInputProps,
  TouchableOpacity,
  View,
  type ViewStyle,
} from 'react-native';

import {
  colors,
  elevation,
  HIT_SLOP,
  MIN_TOUCH,
  radius,
  space,
  type,
} from './theme';

/* --- layout --- */

/** Standard screen frame: a title row with an optional back affordance and
 * trailing slot, over a scrolling body. */
export function Screen({
  title,
  onBack,
  backLabel = 'Back',
  trailing,
  children,
  scroll = true,
}: {
  title: string;
  onBack?: () => void;
  backLabel?: string;
  trailing?: React.ReactNode;
  children: React.ReactNode;
  scroll?: boolean;
}): React.JSX.Element {
  const header = (
    <View style={styles.screenHeader}>
      <View style={styles.screenHeaderMain}>
        {onBack && (
          <TouchableOpacity
            onPress={onBack}
            hitSlop={HIT_SLOP}
            accessibilityRole="button"
            accessibilityLabel={backLabel}
            style={styles.backButton}
          >
            <Text style={styles.backChevron}>‹</Text>
          </TouchableOpacity>
        )}
        <Text style={styles.screenTitle} numberOfLines={1}>
          {title}
        </Text>
      </View>
      {trailing}
    </View>
  );

  if (!scroll) {
    return (
      <View style={styles.screen}>
        {header}
        <View style={styles.screenBodyStatic}>{children}</View>
      </View>
    );
  }
  return (
    <View style={styles.screen}>
      {header}
      <ScrollView
        style={styles.screenScroll}
        contentContainerStyle={styles.screenBody}
        keyboardShouldPersistTaps="handled"
      >
        {children}
      </ScrollView>
    </View>
  );
}

/** A grouped surface. `padded={false}` for cards whose children manage their
 * own insets (e.g. a list with full-width dividers). */
export function Card({
  children,
  style,
  padded = true,
}: {
  children: React.ReactNode;
  style?: ViewStyle;
  padded?: boolean;
}): React.JSX.Element {
  return (
    <View style={[styles.card, padded && styles.cardPadded, style]}>
      {children}
    </View>
  );
}

export function SectionHeader({
  children,
  hint,
}: {
  children: string;
  hint?: string;
}): React.JSX.Element {
  return (
    <View style={styles.sectionHeader}>
      <Text style={styles.overline}>{children}</Text>
      {hint && <Text style={styles.caption}>{hint}</Text>}
    </View>
  );
}

/** Thin separator for rows inside a Card. */
export function Divider(): React.JSX.Element {
  return <View style={styles.divider} />;
}

/* --- controls --- */

type ButtonTone = 'primary' | 'secondary' | 'danger' | 'ghost';

export function Button({
  label,
  onPress,
  tone = 'primary',
  disabled,
  busy,
  style,
  accessibilityLabel,
}: {
  label: string;
  onPress: () => void;
  tone?: ButtonTone;
  disabled?: boolean;
  busy?: boolean;
  style?: ViewStyle;
  accessibilityLabel?: string;
}): React.JSX.Element {
  const isDisabled = disabled || busy;
  return (
    <Pressable
      onPress={onPress}
      disabled={isDisabled}
      accessibilityRole="button"
      accessibilityLabel={accessibilityLabel ?? label}
      accessibilityState={{ disabled: !!isDisabled, busy: !!busy }}
      style={({ pressed }) => [
        styles.button,
        toneStyles[tone].container,
        pressed && !isDisabled && styles.buttonPressed,
        isDisabled && styles.disabled,
        style,
      ]}
    >
      {busy && (
        <ActivityIndicator
          size="small"
          color={toneStyles[tone].label.color}
          style={styles.buttonSpinner}
        />
      )}
      <Text
        style={[styles.buttonLabel, toneStyles[tone].label]}
        numberOfLines={1}
      >
        {label}
      </Text>
    </Pressable>
  );
}

/** Pill-shaped multi/single select. Used anywhere a <Picker> would be — bare
 * RN has no picker in core and a community one is another native dependency
 * (see PinMapperScreen's header). */
export function Chip({
  label,
  active,
  onPress,
  tone = 'accent',
  disabled,
  accessibilityLabel,
}: {
  label: string;
  active?: boolean;
  onPress?: () => void;
  tone?: 'accent' | 'on' | 'danger' | 'neutral';
  disabled?: boolean;
  accessibilityLabel?: string;
}): React.JSX.Element {
  const activeStyle = chipTones[tone];
  const content = (
    <Text
      style={[styles.chipLabel, active && { color: activeStyle.text }]}
      numberOfLines={1}
    >
      {label}
    </Text>
  );
  if (!onPress) {
    return (
      <View style={[styles.chip, active && activeStyle.container]}>
        {content}
      </View>
    );
  }
  return (
    <Pressable
      onPress={onPress}
      disabled={disabled}
      hitSlop={HIT_SLOP}
      accessibilityRole="button"
      accessibilityLabel={accessibilityLabel ?? label}
      accessibilityState={{ selected: !!active, disabled: !!disabled }}
      style={({ pressed }) => [
        styles.chip,
        active && activeStyle.container,
        pressed && !disabled && styles.chipPressed,
        disabled && styles.disabled,
      ]}
    >
      {content}
    </Pressable>
  );
}

/** Dropdown select. Bare RN has no picker in core and a community one is
 * another native dependency this project can't verify (see PinMapperScreen's
 * header), so this is a Modal-backed sheet: a closed row showing the current
 * value, opening to a list of options with the selected one marked.
 *
 * Used instead of chip rows wherever the choices are mutually exclusive and
 * more than about three — chips imply multi-select and grow unreadably wide. */
export function Select<T extends string>({
  label,
  value,
  options,
  onChange,
  hint,
  disabled,
}: {
  label?: string;
  value: T;
  options: readonly { value: T; label: string; hint?: string }[];
  onChange: (value: T) => void;
  hint?: string;
  disabled?: boolean;
}): React.JSX.Element {
  const [open, setOpen] = React.useState(false);
  const current = options.find(o => o.value === value);

  return (
    <View style={styles.field}>
      {label && <Text style={styles.fieldLabel}>{label}</Text>}
      <Pressable
        onPress={() => setOpen(true)}
        disabled={disabled}
        accessibilityRole="button"
        accessibilityLabel={`${label ?? 'Select'}: ${current?.label ?? value}`}
        accessibilityHint="Opens a list of choices"
        style={({ pressed }) => [
          styles.selectRow,
          pressed && !disabled && styles.chipPressed,
          disabled && styles.disabled,
        ]}
      >
        <Text style={styles.selectValue} numberOfLines={1}>
          {current?.label ?? value}
        </Text>
        <Text style={styles.selectChevron}>⌄</Text>
      </Pressable>
      {hint && <Text style={styles.caption}>{hint}</Text>}

      <Modal
        visible={open}
        transparent
        animationType="fade"
        onRequestClose={() => setOpen(false)}
      >
        {/* Tapping the scrim dismisses, which is what every native picker
         * does and what a rider will try first. */}
        <Pressable
          style={styles.modalScrim}
          onPress={() => setOpen(false)}
          accessibilityLabel="Close"
        >
          <Pressable style={styles.modalSheet} onPress={() => {}}>
            {label && (
              <Text style={[styles.fieldLabel, styles.modalTitle]}>
                {label}
              </Text>
            )}
            <ScrollView>
              {options.map(o => {
                const selected = o.value === value;
                return (
                  <Pressable
                    key={o.value}
                    onPress={() => {
                      onChange(o.value);
                      setOpen(false);
                    }}
                    accessibilityRole="button"
                    accessibilityState={{ selected }}
                    accessibilityLabel={o.label}
                    style={({ pressed }) => [
                      styles.optionRow,
                      pressed && styles.chipPressed,
                    ]}
                  >
                    <View style={styles.flexOne}>
                      <Text
                        style={[
                          styles.optionLabel,
                          selected && styles.optionLabelSelected,
                        ]}
                      >
                        {o.label}
                      </Text>
                      {o.hint && <Text style={styles.caption}>{o.hint}</Text>}
                    </View>
                    {selected && <Text style={styles.optionTick}>✓</Text>}
                  </Pressable>
                );
              })}
            </ScrollView>
          </Pressable>
        </Pressable>
      </Modal>
    </View>
  );
}

/** A labelled on/off switch row, for the role flags. Whole row is the target
 * so it's usable with gloves, not just the switch itself. */
export function ToggleRow({
  label,
  hint,
  value,
  onValueChange,
  disabled,
}: {
  label: string;
  hint?: string;
  value: boolean;
  onValueChange: (v: boolean) => void;
  disabled?: boolean;
}): React.JSX.Element {
  return (
    <Pressable
      onPress={() => !disabled && onValueChange(!value)}
      accessibilityRole="switch"
      accessibilityState={{ checked: value, disabled: !!disabled }}
      accessibilityLabel={label}
      accessibilityHint={hint}
      style={({ pressed }) => [
        styles.toggleRow,
        pressed && !disabled && styles.chipPressed,
        disabled && styles.disabled,
      ]}
    >
      <View style={styles.flexOne}>
        <Text style={styles.toggleLabel}>{label}</Text>
        {hint && <Text style={styles.caption}>{hint}</Text>}
      </View>
      <Switch
        value={value}
        onValueChange={onValueChange}
        disabled={disabled}
        trackColor={{ false: colors.borderStrong, true: colors.accent }}
        thumbColor={colors.text}
        ios_backgroundColor={colors.borderStrong}
      />
    </Pressable>
  );
}

export function Field({
  label,
  hint,
  ...inputProps
}: { label?: string; hint?: string } & TextInputProps): React.JSX.Element {
  return (
    <View style={styles.field}>
      {label && <Text style={styles.fieldLabel}>{label}</Text>}
      <TextInput
        placeholderTextColor={colors.textFaint}
        accessibilityLabel={label}
        {...inputProps}
        style={[styles.input, inputProps.style]}
      />
      {hint && <Text style={styles.caption}>{hint}</Text>}
    </View>
  );
}

/* --- display --- */

export function Stat({
  label,
  value,
  tone,
}: {
  label: string;
  value: string;
  tone?: 'on' | 'warn' | 'danger';
}): React.JSX.Element {
  return (
    <View style={styles.stat}>
      <Text style={styles.overline}>{label}</Text>
      <Text
        style={[styles.statValue, tone && { color: colors[tone] }]}
        numberOfLines={1}
      >
        {value}
      </Text>
    </View>
  );
}

export function Badge({
  label,
  tone = 'neutral',
}: {
  label: string;
  tone?: 'neutral' | 'on' | 'warn' | 'danger' | 'info' | 'accent';
}): React.JSX.Element {
  return (
    <View style={[styles.badge, badgeTones[tone].container]}>
      <Text style={[styles.badgeLabel, badgeTones[tone].label]}>{label}</Text>
    </View>
  );
}

/** Inline message. `tone` carries the meaning; the icon is decorative and
 * hidden from screen readers, which get the text and the role instead. */
export function Notice({
  tone,
  children,
}: {
  tone: 'warn' | 'danger' | 'info' | 'on';
  children: string;
}): React.JSX.Element {
  const glyph = { warn: '!', danger: '!', info: 'i', on: '✓' }[tone];
  return (
    <View
      style={[
        styles.notice,
        {
          backgroundColor: colors[`${tone}Muted` as const],
          borderColor: colors[tone],
        },
      ]}
      accessibilityRole="alert"
    >
      <Text
        style={[styles.noticeGlyph, { color: colors[tone] }]}
        accessibilityElementsHidden
        importantForAccessibility="no"
      >
        {glyph}
      </Text>
      <Text style={styles.noticeText}>{children}</Text>
    </View>
  );
}

export function EmptyState({
  title,
  body,
}: {
  title: string;
  body?: string;
}): React.JSX.Element {
  return (
    <View style={styles.empty}>
      <Text style={styles.emptyTitle}>{title}</Text>
      {body && <Text style={[styles.caption, styles.emptyBody]}>{body}</Text>}
    </View>
  );
}

export function Loading({ label }: { label?: string }): React.JSX.Element {
  return (
    <View style={styles.loading}>
      <ActivityIndicator color={colors.accent} />
      {label && <Text style={styles.caption}>{label}</Text>}
    </View>
  );
}

/* --- styles --- */

const toneStyles: Record<
  ButtonTone,
  { container: ViewStyle; label: { color: string } }
> = {
  primary: {
    container: { backgroundColor: colors.accent },
    label: { color: colors.textOnAccent },
  },
  secondary: {
    container: {
      backgroundColor: colors.raisedHover,
      borderWidth: 1,
      borderColor: colors.borderStrong,
    },
    label: { color: colors.text },
  },
  danger: {
    container: { backgroundColor: colors.danger },
    label: { color: colors.textOnAccent },
  },
  ghost: {
    container: { backgroundColor: 'transparent' },
    label: { color: colors.textMuted },
  },
};

const chipTones = {
  accent: {
    container: {
      backgroundColor: colors.accentMuted,
      borderColor: colors.accentBorder,
    },
    text: colors.accent,
  },
  on: {
    container: { backgroundColor: colors.onMuted, borderColor: colors.on },
    text: colors.on,
  },
  danger: {
    container: {
      backgroundColor: colors.dangerMuted,
      borderColor: colors.danger,
    },
    text: colors.danger,
  },
  neutral: {
    container: {
      backgroundColor: colors.raisedHover,
      borderColor: colors.borderStrong,
    },
    text: colors.text,
  },
} as const;

const badgeTones = {
  neutral: {
    container: { backgroundColor: colors.raisedHover },
    label: { color: colors.textMuted },
  },
  on: {
    container: { backgroundColor: colors.onMuted },
    label: { color: colors.on },
  },
  warn: {
    container: { backgroundColor: colors.warnMuted },
    label: { color: colors.warn },
  },
  danger: {
    container: { backgroundColor: colors.dangerMuted },
    label: { color: colors.danger },
  },
  info: {
    container: { backgroundColor: colors.infoMuted },
    label: { color: colors.info },
  },
  accent: {
    container: { backgroundColor: colors.accentMuted },
    label: { color: colors.accent },
  },
} as const;

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: colors.bg },
  screenHeader: {
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    paddingHorizontal: space.lg,
    paddingTop: space.sm,
    paddingBottom: space.md,
    gap: space.sm,
  },
  screenHeaderMain: {
    flexDirection: 'row',
    alignItems: 'center',
    flexShrink: 1,
    gap: space.xs,
  },
  backButton: {
    width: 32,
    height: MIN_TOUCH,
    alignItems: 'flex-start',
    justifyContent: 'center',
  },
  backChevron: {
    fontSize: 34,
    lineHeight: 38,
    color: colors.accent,
    fontWeight: '300',
  },
  screenTitle: { ...type.title, flexShrink: 1 },
  screenScroll: { flex: 1 },
  screenBody: {
    paddingHorizontal: space.lg,
    paddingBottom: space.xxl,
    gap: space.md,
  },
  screenBodyStatic: {
    flex: 1,
    paddingHorizontal: space.lg,
    paddingBottom: space.lg,
    gap: space.md,
  },

  card: {
    backgroundColor: colors.raised,
    borderRadius: radius.lg,
    borderWidth: 1,
    borderColor: colors.border,
    ...elevation.card,
  },
  cardPadded: { padding: space.md, gap: space.sm },
  sectionHeader: { gap: space.xs, marginTop: space.sm },
  overline: type.overline,
  caption: type.caption,
  divider: { height: 1, backgroundColor: colors.border },

  button: {
    minHeight: MIN_TOUCH,
    borderRadius: radius.md,
    alignItems: 'center',
    justifyContent: 'center',
    flexDirection: 'row',
    paddingHorizontal: space.lg,
    gap: space.sm,
  },
  buttonPressed: { opacity: 0.8 },
  buttonLabel: { fontSize: 15, fontWeight: '600' },
  buttonSpinner: { marginRight: 0 },
  disabled: { opacity: 0.4 },

  chip: {
    minHeight: 34,
    paddingHorizontal: space.md,
    justifyContent: 'center',
    borderRadius: radius.pill,
    borderWidth: 1,
    borderColor: colors.border,
    backgroundColor: colors.sunken,
  },
  chipPressed: { opacity: 0.7 },
  chipLabel: { fontSize: 13, color: colors.textMuted, fontWeight: '500' },

  flexOne: { flex: 1 },
  field: { gap: space.xs },
  selectRow: {
    minHeight: MIN_TOUCH,
    flexDirection: 'row',
    alignItems: 'center',
    justifyContent: 'space-between',
    borderWidth: 1,
    borderColor: colors.borderStrong,
    backgroundColor: colors.sunken,
    borderRadius: radius.md,
    paddingHorizontal: space.md,
    gap: space.sm,
  },
  selectValue: { ...type.body, flexShrink: 1 },
  selectChevron: { fontSize: 16, color: colors.textFaint },
  modalScrim: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.6)',
    justifyContent: 'flex-end',
  },
  modalSheet: {
    backgroundColor: colors.raised,
    borderTopLeftRadius: radius.lg,
    borderTopRightRadius: radius.lg,
    borderTopWidth: 1,
    borderColor: colors.border,
    paddingVertical: space.md,
    paddingHorizontal: space.lg,
    maxHeight: '70%',
  },
  modalTitle: { marginBottom: space.sm },
  optionRow: {
    minHeight: MIN_TOUCH,
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    paddingVertical: space.sm,
  },
  optionLabel: { ...type.body },
  optionLabelSelected: { color: colors.accent, fontWeight: '600' },
  optionTick: { color: colors.accent, fontSize: 16, fontWeight: '700' },
  toggleRow: {
    minHeight: MIN_TOUCH,
    flexDirection: 'row',
    alignItems: 'center',
    gap: space.md,
    paddingVertical: space.xs,
  },
  toggleLabel: { ...type.body, fontWeight: '600' },
  fieldLabel: { ...type.overline },
  input: {
    minHeight: MIN_TOUCH,
    borderWidth: 1,
    borderColor: colors.borderStrong,
    backgroundColor: colors.sunken,
    borderRadius: radius.md,
    paddingHorizontal: space.md,
    color: colors.text,
    fontSize: 15,
  },

  stat: {
    flexGrow: 1,
    flexBasis: '46%',
    backgroundColor: colors.raised,
    borderRadius: radius.md,
    borderWidth: 1,
    borderColor: colors.border,
    padding: space.md,
    gap: space.xs,
  },
  statValue: type.value,

  badge: {
    paddingHorizontal: space.sm,
    paddingVertical: 3,
    borderRadius: radius.sm,
    alignSelf: 'flex-start',
  },
  badgeLabel: { fontSize: 11, fontWeight: '700', letterSpacing: 0.4 },

  notice: {
    flexDirection: 'row',
    gap: space.sm,
    borderRadius: radius.md,
    borderLeftWidth: 3,
    borderWidth: 0,
    padding: space.md,
    alignItems: 'flex-start',
  },
  noticeGlyph: {
    fontSize: 13,
    fontWeight: '800',
    width: 12,
    textAlign: 'center',
  },
  noticeText: { ...type.caption, flex: 1, color: colors.text },

  empty: { alignItems: 'center', padding: space.xl, gap: space.xs },
  emptyTitle: { ...type.body, color: colors.textMuted, fontWeight: '600' },
  emptyBody: { textAlign: 'center' },

  loading: {
    flex: 1,
    alignItems: 'center',
    justifyContent: 'center',
    gap: space.md,
  },
});
