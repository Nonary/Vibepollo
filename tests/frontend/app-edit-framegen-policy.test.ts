import { describe, expect, it } from 'vitest';
import {
  frameGenDisplayHealthKey,
  frameGenDisplayNotice,
  physicalFrameGenDisplayWarningKey,
  resolvesToVirtualDisplay,
} from '@web/components/app-edit/frameGenDisplayPolicy';

const PHYSICAL_KEY = 'config.physical_display_framegen_warning';
const VIRTUAL_GAME_PROVIDED_KEY = 'apps.framegen.notice_virtual_game_provided';
const VIRTUAL_GENERIC_KEY = 'apps.framegen.notice_virtual_generic';

describe('app edit frame generation display policy', () => {
  it('treats global virtual-display inheritance as virtual', () => {
    expect(
      resolvesToVirtualDisplay({
        displaySelection: 'global',
        appVirtualDisplayMode: null,
        globalVirtualDisplayMode: 'per_client',
        globalOutputName: '',
      }),
    ).toBe(true);
  });

  it('treats explicit physical-display apps as not virtual', () => {
    expect(
      resolvesToVirtualDisplay({
        displaySelection: 'physical',
        appVirtualDisplayMode: 'disabled',
        globalVirtualDisplayMode: 'per_client',
        globalOutputName: '',
      }),
    ).toBe(false);
  });

  it('exposes the shared physical-display warning key', () => {
    expect(physicalFrameGenDisplayWarningKey()).toBe(PHYSICAL_KEY);
  });

  it('returns no app-edit banner when frame generation is off', () => {
    expect(frameGenDisplayNotice(true, 'off')).toBeNull();
    expect(frameGenDisplayNotice(false, 'off')).toBeNull();
  });

  it('affirms virtual-display capture for game-provided frame generation', () => {
    expect(frameGenDisplayNotice(true, 'game-provided')).toEqual({
      type: 'info',
      key: VIRTUAL_GAME_PROVIDED_KEY,
    });
  });

  it('describes automatic pacing for other virtual-display frame generation', () => {
    expect(frameGenDisplayNotice(true, 'lossless-scaling')).toEqual({
      type: 'info',
      key: VIRTUAL_GENERIC_KEY,
    });
  });

  it('warns about high latency for physical-display frame generation', () => {
    expect(frameGenDisplayNotice(false, 'game-provided')).toEqual({
      type: 'warning',
      key: PHYSICAL_KEY,
    });
    expect(frameGenDisplayNotice(false, 'lossless-scaling')).toEqual({
      type: 'warning',
      key: PHYSICAL_KEY,
    });
  });

  it('returns the redesigned display health message keys', () => {
    expect(frameGenDisplayHealthKey(true, 'game-provided')).toBe(VIRTUAL_GAME_PROVIDED_KEY);
    expect(frameGenDisplayHealthKey(true, 'nvidia-smooth-motion')).toBe(VIRTUAL_GENERIC_KEY);
    expect(frameGenDisplayHealthKey(false, 'game-provided')).toBe(PHYSICAL_KEY);
    expect(frameGenDisplayHealthKey(false, 'off')).toBe(PHYSICAL_KEY);
  });
});
