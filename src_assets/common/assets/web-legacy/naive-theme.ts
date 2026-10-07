import type { GlobalThemeOverrides } from 'naive-ui';
import { ref, onMounted, onBeforeUnmount, watch } from 'vue';

// Use your existing CSS variables to keep the current color scheme.
// Naive UI accepts any valid CSS color string, so we reference the
// same tokens to maintain visual consistency across light/dark.

// Resolve `--color-xxx` (space-separated RGB like "77 163 255") to 'rgb(r, g, b)'
function cssVarRgb(name: string, fallback: string): string {
  if (typeof window === 'undefined') return fallback;
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  if (!raw) return fallback;
  // Accept formats like "77 163 255" or "77, 163, 255"
  const parts = raw.replace(/\s+/g, ' ').replace(/,/g, ' ').trim().split(' ');
  if (parts.length < 3) return fallback;
  const [r, g, b] = parts;
  const nr = Number(r),
    ng = Number(g),
    nb = Number(b);
  if ([nr, ng, nb].some((n) => !isFinite(n))) return fallback;
  return `rgb(${nr}, ${ng}, ${nb})`;
}

// Resolve `--color-xxx` to a comma-separated "r, g, b" string for rgba()
function cssVarRgbComma(name: string, fallback: string): string {
  if (typeof window === 'undefined') return fallback;
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  if (!raw) return fallback;
  const parts = raw.replace(/\s+/g, ' ').replace(/,/g, ' ').trim().split(' ');
  if (parts.length < 3) return fallback;
  const [r, g, b] = parts;
  const nr = Number(r),
    ng = Number(g),
    nb = Number(b);
  if ([nr, ng, nb].some((n) => !isFinite(n))) return fallback;
  return `${nr}, ${ng}, ${nb}`;
}

export function useNaiveThemeOverrides() {
  const overrides = ref<GlobalThemeOverrides>({});
  const clamp = (n: number) => Math.max(0, Math.min(255, Math.round(n)));
  const parse = (rgb: string): [number, number, number] => {
    const m = rgb.match(/(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if (m) return [Number(m[1]), Number(m[2]), Number(m[3])];
    const mm = rgb.match(/(\d+)\s+(\d+)\s+(\d+)/);
    if (mm) return [Number(mm[1]), Number(mm[2]), Number(mm[3])];
    return [0, 0, 0];
  };
  const toCss = (r: number, g: number, b: number) => `rgb(${clamp(r)}, ${clamp(g)}, ${clamp(b)})`;
  const lighten = (rgb: string, amt: number) => {
    const [r, g, b] = parse(rgb);
    return toCss(r + (255 - r) * amt, g + (255 - g) * amt, b + (255 - b) * amt);
  };
  const darken = (rgb: string, amt: number) => {
    const [r, g, b] = parse(rgb);
    return toCss(r * (1 - amt), g * (1 - amt), b * (1 - amt));
  };
  const compute = () => {
    const primary = cssVarRgb('--color-primary', '77, 163, 255');
    const info = cssVarRgb('--color-info', '2, 136, 209');
    const success = cssVarRgb('--color-success', '76, 175, 80');
    const warning = cssVarRgb('--color-warning', '245, 124, 0');
    const danger = cssVarRgb('--color-danger', '220, 38, 38');
    overrides.value = {
      common: {
        primaryColor: primary,
        primaryColorHover: darken(primary, 0.08),
        primaryColorPressed: darken(primary, 0.16),
        primaryColorSuppl: lighten(primary, 0.12),
        infoColor: info,
        infoColorHover: darken(info, 0.08),
        infoColorPressed: darken(info, 0.16),
        infoColorSuppl: lighten(info, 0.12),
        successColor: success,
        successColorHover: darken(success, 0.08),
        successColorPressed: darken(success, 0.16),
        successColorSuppl: lighten(success, 0.12),
        warningColor: warning,
        warningColorHover: darken(warning, 0.08),
        warningColorPressed: darken(warning, 0.16),
        warningColorSuppl: lighten(warning, 0.12),
        errorColor: danger,
        errorColorHover: darken(danger, 0.08),
        errorColorPressed: darken(danger, 0.16),
        errorColorSuppl: lighten(danger, 0.12),

        baseColor: cssVarRgb('--color-light', '#ffffff'),
        bodyColor: cssVarRgb('--color-light', '#ffffff'),
        textColorBase: cssVarRgb('--color-dark', '#000000'),
        cardColor: cssVarRgb('--color-surface', '#ffffff'),
        modalColor: cssVarRgb('--color-surface', '#ffffff'),
        popoverColor: cssVarRgb('--color-surface', '#ffffff'),
        tableColor: cssVarRgb('--color-light', '#ffffff'),

        // Subtle borders/dividers using resolved theme tokens (avoid var() usage here)
        borderColor: `rgba(${cssVarRgbComma('--color-dark', '0, 0, 0')}, 0.10)`,
        dividerColor: `rgba(${cssVarRgbComma('--color-dark', '0, 0, 0')}, 0.10)`,
      },
    } as GlobalThemeOverrides;
  };

  onMounted(compute);
  // Also export a small hook below that flags dark changes; recompute on changes
  const isDark = useDarkModeClassRef();
  watch(isDark, () => compute());

  return overrides;
}

// Small helper to sync Naive's theme with your existing dark-mode class.
// Usage: const isDark = useDarkModeClass();
export function useDarkModeClassRef() {
  const isDark = ref<boolean>(false);
  let observer: MutationObserver | null = null;

  const update = () => {
    if (typeof document !== 'undefined') {
      isDark.value = document.documentElement.classList.contains('dark');
    }
  };

  if (typeof window !== 'undefined') {
    update();
    onMounted(() => {
      update();
      observer = new MutationObserver(update);
      observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] });
    });
    onBeforeUnmount(() => {
      observer?.disconnect();
      observer = null;
    });
  }

  return isDark;
}
