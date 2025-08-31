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
  plugins: [],
};
