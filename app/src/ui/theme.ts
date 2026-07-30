/**
 * Design tokens for the MOTO-CTRL app.
 *
 * Single source of truth for colour, spacing, radius and type. Screens import
 * from here instead of hardcoding hex values, so a change lands everywhere at
 * once.
 *
 * Dark-first, deliberately: this is a companion app for a machine, usually
 * opened in a garage or at the roadside, often at night, and a dark surface
 * keeps a phone from blinding the rider. The palette is a cool neutral grey
 * with a single warm accent, so status colours (green/amber/red) read as
 * meaningful rather than decorative.
 *
 * Semantic names only — `danger`, not `red`. A control that switches a
 * motorcycle's ignition should say what it means at the call site.
 */
import { Platform } from 'react-native';

export const colors = {
  /* Surfaces, darkest to lightest. `bg` is the app background; `raised` is a
   * card on top of it; `sunken` is an inset well (inputs, code). */
  bg: '#0f1115',
  raised: '#181b22',
  raisedHover: '#1f232c',
  sunken: '#0a0c10',
  border: '#272c37',
  borderStrong: '#39404f',

  /* Text, in descending emphasis. */
  text: '#f2f4f8',
  textMuted: '#9aa3b2',
  textFaint: '#6b7484',
  textOnAccent: '#0f1115',

  /* Single accent — used for primary actions and active state. Amber reads
   * as "instrument panel" rather than "web app blue". */
  accent: '#f5a524',
  accentMuted: '#4a3a1c',
  accentBorder: '#7a5f24',

  /* Status. `on` doubles as the "output energised" colour. */
  on: '#2dd4a7',
  onMuted: '#12352d',
  danger: '#f87171',
  dangerMuted: '#3d1d1d',
  warn: '#fbbf24',
  warnMuted: '#3a2e12',
  info: '#60a5fa',
  infoMuted: '#1b2a44',
} as const;

/* 4pt grid. Named by role rather than size so layouts stay consistent. */
export const space = {
  xs: 4,
  sm: 8,
  md: 12,
  lg: 16,
  xl: 24,
  xxl: 32,
} as const;

export const radius = {
  sm: 6,
  md: 10,
  lg: 14,
  pill: 999,
} as const;

/* The one place a platform font is chosen. Monospace is reserved for values
 * a rider might read out or compare digit by digit (voltages, versions,
 * channel numbers) — never for prose. */
export const font = {
  mono: Platform.select({
    ios: 'Menlo',
    android: 'monospace',
    default: 'monospace',
  }) as string,
} as const;

export const type = {
  /* Screen title. */
  title: {
    fontSize: 24,
    fontWeight: '700',
    color: colors.text,
    letterSpacing: -0.4,
  },
  /* Card / group heading. */
  heading: { fontSize: 16, fontWeight: '600', color: colors.text },
  /* Small all-caps section divider. */
  overline: {
    fontSize: 11,
    fontWeight: '700',
    color: colors.textFaint,
    letterSpacing: 0.9,
    textTransform: 'uppercase',
  },
  body: { fontSize: 14, color: colors.text },
  bodyMuted: { fontSize: 14, color: colors.textMuted },
  /* Explanatory text under a control. Legibility floor is 12pt. */
  caption: { fontSize: 12, color: colors.textMuted, lineHeight: 17 },
  value: { fontSize: 18, fontFamily: font.mono, color: colors.text },
  valueSmall: { fontSize: 12, fontFamily: font.mono, color: colors.textMuted },
} as const;

/* Elevation. Android needs `elevation`; iOS needs the shadow quartet. Kept
 * subtle — on a dark UI, border contrast does more work than shadow. */
export const elevation = {
  card: Platform.select({
    ios: {
      shadowColor: '#000',
      shadowOpacity: 0.35,
      shadowRadius: 12,
      shadowOffset: { width: 0, height: 4 },
    },
    android: { elevation: 3 },
    default: {},
  }),
} as const;

/** Minimum touch target (iOS HIG 44pt / Material 48dp). Anything a rider taps
 * while wearing gloves should meet this. */
export const HIT_SLOP = { top: 8, bottom: 8, left: 8, right: 8 } as const;
export const MIN_TOUCH = 44;
