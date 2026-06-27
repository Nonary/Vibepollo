import { describe, expect, it } from 'vitest';
import {
  frameGenDisplayHealthMessage,
  frameGenDisplayNotice,
  physicalFrameGenDisplayWarning,
  resolvesToVirtualDisplay,
} from '@web/components/app-edit/frameGenDisplayPolicy';

const PHYSICAL_WARNING =
  'Physical displays cannot capture DLSS/FSR generated frames. If you use DLSS/FSR frame generation with an RTSS frame limiter on a physical screen, latency can increase by 50 ms or more. Without suitable frame pacing, it may micro-stutter or judder. Use a virtual display for low-latency, smooth capture.';
const VIRTUAL_GAME_PROVIDED =
  'Virtual display captures DLSS/FSR generated frames reliably and paces them automatically.';
const VIRTUAL_GENERIC = 'Virtual display uses automatic frame pacing for smoother frame generation.';

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

  it('exposes the shared physical-display warning', () => {
    expect(physicalFrameGenDisplayWarning()).toBe(PHYSICAL_WARNING);
  });

  it('returns no app-edit banner when frame generation is off', () => {
    expect(frameGenDisplayNotice(true, 'off')).toBeNull();
    expect(frameGenDisplayNotice(false, 'off')).toBeNull();
  });

  it('affirms virtual-display capture for game-provided frame generation', () => {
    expect(frameGenDisplayNotice(true, 'game-provided')).toEqual({
      type: 'info',
      message: VIRTUAL_GAME_PROVIDED,
    });
  });

  it('describes automatic pacing for other virtual-display frame generation', () => {
    expect(frameGenDisplayNotice(true, 'lossless-scaling')).toEqual({
      type: 'info',
      message: VIRTUAL_GENERIC,
    });
  });

  it('warns about high latency for physical-display frame generation', () => {
    expect(frameGenDisplayNotice(false, 'game-provided')).toEqual({
      type: 'warning',
      message: PHYSICAL_WARNING,
    });
    expect(frameGenDisplayNotice(false, 'lossless-scaling')).toEqual({
      type: 'warning',
      message: PHYSICAL_WARNING,
    });
  });

  it('returns the redesigned display health messages', () => {
    expect(frameGenDisplayHealthMessage(true, 'game-provided')).toBe(VIRTUAL_GAME_PROVIDED);
    expect(frameGenDisplayHealthMessage(true, 'nvidia-smooth-motion')).toBe(VIRTUAL_GENERIC);
    expect(frameGenDisplayHealthMessage(false, 'game-provided')).toBe(PHYSICAL_WARNING);
    expect(frameGenDisplayHealthMessage(false, 'off')).toBe(PHYSICAL_WARNING);
  });
});
