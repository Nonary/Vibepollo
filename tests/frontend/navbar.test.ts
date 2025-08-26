import { mount } from '@vue/test-utils';
import { vi } from 'vitest';
import Navbar from '@web/Navbar.vue';

vi.mock('@lizardbyte/shared-web/src/js/discord.js', () => ({
  initDiscord: vi.fn(),
}));

describe('Navbar.vue', () => {
  beforeEach(() => {
    vi.resetAllMocks();
  });

  test('hides Playnite link on non-Windows', async () => {
    (global as any).fetch = vi.fn(async () => ({
      ok: true,
      json: async () => ({ platform: 'linux' }),
    }));
    const w = mount(Navbar as any, { global: { mocks: { $t: (k: string) => k } } });
    await new Promise((r) => setTimeout(r, 0));
    expect(w.html()).not.toContain('href="./playnite"');
  });

  test('shows Playnite link on Windows', async () => {
    (global as any).fetch = vi.fn(async () => ({
      ok: true,
      json: async () => ({ platform: 'windows' }),
    }));
    const w = mount(Navbar as any, { global: { mocks: { $t: (k: string) => k } } });
    await new Promise((r) => setTimeout(r, 0));
    expect(w.html()).toContain('href="./playnite"');
  });
});
