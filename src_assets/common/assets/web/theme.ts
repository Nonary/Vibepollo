type Theme = 'light' | 'dark' | 'auto';

const getStoredTheme = (): string | null => localStorage.getItem('theme');
const setStoredTheme = (theme: string): void => localStorage.setItem('theme', theme);

export const getPreferredTheme = (): Theme => {
  const storedTheme = getStoredTheme();
  if (storedTheme === 'light' || storedTheme === 'dark' || storedTheme === 'auto') {
    return storedTheme;
  }

  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
};

// Set theme in a Tailwind-friendly way by toggling the `dark` class on <html>.
const setTheme = (theme: Theme): void => {
  const prefersDark = window.matchMedia('(prefers-color-scheme: dark)').matches;

  if (theme === 'auto') {
    if (prefersDark) {
      document.documentElement.classList.add('dark');
    } else {
      document.documentElement.classList.remove('dark');
    }
    // When theme is 'auto' we still need to set a concrete data-bs-theme
    // value so Bootstrap selectors like [data-bs-theme="dark"] match.
    const resolved = prefersDark ? 'dark' : 'light';
    document.documentElement.setAttribute('data-bs-theme', resolved);
    document.documentElement.setAttribute('data-theme', 'auto');
  } else if (theme === 'dark') {
    document.documentElement.classList.add('dark');
    document.documentElement.setAttribute('data-bs-theme', 'dark');
    document.documentElement.setAttribute('data-theme', 'dark');
  } else {
    document.documentElement.classList.remove('dark');
    document.documentElement.setAttribute('data-bs-theme', 'light');
    document.documentElement.setAttribute('data-theme', 'light');
  }
};

export const showActiveTheme = (theme: Theme, focus = false): void => {
  const themeSwitcher = document.querySelector('#bd-theme');

  if (!themeSwitcher) {
    return;
  }

  const themeSwitcherText = document.querySelector('#bd-theme-text');
  const activeThemeIcon = document.querySelector('.theme-icon-active i');

  // Friendly icon map if the DOM buttons are not present
  const iconMap: Record<Theme, string> = {
    light: 'fa-solid fa-sun',
    dark: 'fa-solid fa-moon',
    auto: 'fa-solid fa-circle-half-stroke',
  };

  // Update any existing buttons that use data-bs-theme-value (legacy)
  const btnToActive = document.querySelector(`[data-bs-theme-value="${theme}"]`);
  if (btnToActive) {
    document.querySelectorAll('[data-bs-theme-value]').forEach((element) => {
      element.classList.remove('active');
      element.setAttribute('aria-pressed', 'false');
    });

    btnToActive.classList.add('active');
    btnToActive.setAttribute('aria-pressed', 'true');

    const iconInside = btnToActive.querySelector('i');
    if (activeThemeIcon && iconInside) {
      activeThemeIcon.className = iconInside.className;
    }
  } else {
    // Fallback: set a sensible icon based on theme
    if (activeThemeIcon) {
      activeThemeIcon.className = iconMap[theme] || iconMap.auto;
    }
  }

  if (themeSwitcherText) {
    const pretty = btnToActive ? btnToActive.textContent?.trim() : theme;
    const themeSwitcherLabel = `${themeSwitcherText.textContent} (${pretty})`;
    themeSwitcher.setAttribute('aria-label', themeSwitcherLabel);
  }

  if (focus && 'focus' in themeSwitcher) {
    (themeSwitcher as HTMLElement).focus();
  }
};

export function setupThemeToggleListener(): void {
  document.querySelectorAll('[data-bs-theme-value]').forEach((toggle) => {
    toggle.addEventListener('click', () => {
      const theme = toggle.getAttribute('data-bs-theme-value') as Theme;
      if (theme) {
        setStoredTheme(theme);
        setTheme(theme);
        showActiveTheme(theme, true);
      }
    });
  });

  showActiveTheme(getPreferredTheme(), false);
}

export function loadAutoTheme(): void {
  (() => {
    'use strict';

    setTheme(getPreferredTheme());

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
      const storedTheme = getStoredTheme();
      if (storedTheme !== 'light' && storedTheme !== 'dark') {
        setTheme(getPreferredTheme());
      }
    });

    window.addEventListener('DOMContentLoaded', () => {
      showActiveTheme(getPreferredTheme());
    });
  })();
}

// Expose setters so components can call them directly
export { setStoredTheme, setTheme };
