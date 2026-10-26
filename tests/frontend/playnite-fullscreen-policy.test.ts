import { describe, expect, it } from 'vitest';
import { preservePlayniteFullscreen } from '@/utils/playniteFullscreenPolicy';

describe('legacy editor Playnite fullscreen flag', () => {
  it('keeps the flag when an unrelated setting is saved', () => {
    const payload: Record<string, unknown> = { name: 'Playnite (Fullscreen)', 'terminate-on-pause': true };
    preservePlayniteFullscreen({ 'playnite-fullscreen': true }, payload);
    expect(payload['playnite-fullscreen']).toBe(true);
  });

  it('adds nothing for apps without the flag', () => {
    const payload: Record<string, unknown> = { name: 'Desktop' };
    preservePlayniteFullscreen({}, payload);
    preservePlayniteFullscreen(null, payload);
    expect(payload).not.toHaveProperty('playnite-fullscreen');
  });
});
