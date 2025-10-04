import { vi } from 'vitest';

// Minimal i18n mock for components expecting $t
Object.defineProperty(global, '$t', { value: (k: string) => k, writable: true });

// JSDOM fetch mock (override in individual tests as needed)
if (!(global as any).fetch) {
  (global as any).fetch = vi.fn(async (url: string) => {
    return {
      ok: true,
      status: 200,
      json: async () => ({}),
      text: async () => '',
    };
  });
}

// Silence Vue warnings in tests
// @ts-ignore
console.warn = (...args) => {
  if (typeof args[0] === 'string' && args[0].includes('received an unexpected slot')) return;
  return (globalThis as any).__origWarn ? (globalThis as any).__origWarn(...args) : undefined;
};
