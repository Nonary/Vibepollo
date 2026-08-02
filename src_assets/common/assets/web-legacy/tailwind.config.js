const plugin = require('tailwindcss/plugin');

/** @type {import('tailwindcss').Config} */
module.exports = {
  darkMode: 'class',
  // Limit scanning to source files; avoid node_modules for performance
  content: [
    './index.html',
    './*.{vue,js,ts,html}',
    './components/**/*.{vue,js,ts}',
    './views/**/*.{vue,js,ts}',
    './configs/**/*.{vue,js,ts}',
    './stores/**/*.{js,ts}',
  ],
  theme: {
    extend: {
      // Single source of truth for semantic colors (light/dark).
      // These values drive CSS variables that both Tailwind utilities and Naive UI consume.
      semanticColors: {
        light: {
          // Sun-like palette (warmer, less brown)
          primary: '253 184 19', // #FDB813 sun gold
          secondary: '234 88 12', // #EA580C vibrant orange
          success: '76 175 80',
          warning: '245 158 11', // #F59E0B amber
          danger: '220 38 38',
          info: '2 136 209',
          light: '255 250 244', // warm paper
          dark: '33 33 33',
          surface: '255 248 225', // #FFF8E1 soft cream
          accent: '245 130 0', // #F58200 rich amber accent
          onPrimary: '33 33 33', // dark text on bright gold for contrast
          onSecondary: '255 255 255',
          onAccent: '255 255 255',
          onLight: '33 33 33',
          onDark: '255 255 255',
          brand: '217 119 6', // #D97706 brand orange
        },
        dark: {
          // 🌙 Lunar (cosmic blues + purples), tuned for contrast on deep navy
          dark: '6 10 24', // #0A0F18  starfield background
          surface: '14 20 36', // #0E1424  panels/cards
          light: '224 236 255', // #E0ECFF  pale moon haze (for light surfaces)

          // Core actions/brand (cool spectrum)
          primary: '99 102 241', // #6366F1  indigo (main CTA/links)
          secondary: '168 85 247', // #A855F7  vibrant purple (alt CTA/accents)
          accent: '56 189 248', // #38BDF8  cyan comet (chips/highlights)
          info: '147 197 253', // #93C5FD  periwinkle info

          // Status with lunar tilt (cool-leaning where possible)
          success: '16 185 129', // #10B981  emerald-teal
          warning: '245 158 11', // #F59E0B  amber flare (kept warm for salience)
          danger: '225 29 72', // #E11D48  rose/magenta-leaning red

          // Text-on-color (picked to pass AA on typical sizes)
          onDark: '245 249 255', // near-white on dark/surface
          onSurface: '245 249 255', // (if you expose separately)
          onLight: '6 10 24', // dark text on light surfaces
          onPrimary: '245 249 255', // white on indigo (AA+)
          onSecondary: '245 249 255', // white on purple (AA+)
          onAccent: '6 10 24', // dark text on cyan (AA+)
          onInfo: '6 10 24', // dark text on light info blue (AA+)

          // Brand tint (cool lavender for logos/illustrations)
          brand: '165 180 252', // #A5B4FC  steel-lavender
        },
      },
      colors: {
        // Semantic tokens resolved via CSS variables (light defaults, dark overrides via .dark)
        primary: 'rgb(var(--color-primary) / <alpha-value>)',
        secondary: 'rgb(var(--color-secondary) / <alpha-value>)',
        success: 'rgb(var(--color-success) / <alpha-value>)',
        warning: 'rgb(var(--color-warning) / <alpha-value>)',
        danger: 'rgb(var(--color-danger) / <alpha-value>)',
        info: 'rgb(var(--color-info) / <alpha-value>)',
        light: 'rgb(var(--color-light) / <alpha-value>)',
        dark: 'rgb(var(--color-dark) / <alpha-value>)',
        surface: 'rgb(var(--color-surface) / <alpha-value>)',
        accent: 'rgb(var(--color-accent) / <alpha-value>)',
        onPrimary: 'rgb(var(--color-on-primary) / <alpha-value>)',
        onSecondary: 'rgb(var(--color-on-secondary) / <alpha-value>)',
        onAccent: 'rgb(var(--color-on-accent) / <alpha-value>)',
        onLight: 'rgb(var(--color-on-light) / <alpha-value>)',
        onDark: 'rgb(var(--color-on-dark) / <alpha-value>)',
        // Optional brand token for places that previously mixed solar-secondary + lunar-onSecondary
        brand: 'rgb(var(--color-brand) / <alpha-value>)',
      },
    },
  },
  // Enable Tailwind preflight now that Bootstrap is removed. Keep visibility disabled if not needed.
  corePlugins: {
    preflight: true,
    visibility: false,
  },
  plugins: [
    // Emit CSS variables for semantic tokens from theme.semanticColors
    plugin(function ({ addBase, theme }) {
      const light = theme('semanticColors.light') || {};
      const dark = theme('semanticColors.dark') || {};
      const toVars = (src) => ({
        '--color-primary': src.primary,
        '--color-secondary': src.secondary,
        '--color-success': src.success,
        '--color-warning': src.warning,
        '--color-danger': src.danger,
        '--color-info': src.info,
        '--color-light': src.light,
        '--color-dark': src.dark,
        '--color-surface': src.surface,
        '--color-accent': src.accent,
        '--color-on-primary': src.onPrimary,
        '--color-on-secondary': src.onSecondary,
        '--color-on-accent': src.onAccent,
        '--color-on-light': src.onLight,
        '--color-on-dark': src.onDark,
        '--color-brand': src.brand,
      });
      addBase({
        ':root': toVars(light),
        '.dark': toVars(dark),
      });
    }),
  ],
};
