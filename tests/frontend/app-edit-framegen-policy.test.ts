import { describe, expect, it } from 'vitest';
import {
  frameGenDisplayHealthMessage,
  resolvesToVirtualDisplay,
  shouldAutoEnableCaptureFixForFrameGeneration,
  shouldPersistFrameGenerationCaptureFix,
} from '@web/components/app-edit/frameGenDisplayPolicy';

describe('app edit frame generation display policy', () => {
  it('treats global virtual-display inheritance as virtual and clears persisted capture fix', () => {
    const usesVirtual = resolvesToVirtualDisplay({
      displaySelection: 'global',
      appVirtualDisplayMode: null,
      globalVirtualDisplayMode: 'per_client',
      globalOutputName: '',
    });

    expect(usesVirtual).toBe(true);
    expect(shouldPersistFrameGenerationCaptureFix(true, usesVirtual)).toBe(false);
  });

  it('clears persisted capture fix for physical-display apps', () => {
    const usesVirtual = resolvesToVirtualDisplay({
      displaySelection: 'physical',
      appVirtualDisplayMode: 'disabled',
      globalVirtualDisplayMode: 'per_client',
      globalOutputName: '',
    });

    expect(usesVirtual).toBe(false);
    expect(shouldPersistFrameGenerationCaptureFix(true, usesVirtual)).toBe(false);
  });

  it('does not auto-enable capture fix for frame generation', () => {
    expect(shouldAutoEnableCaptureFixForFrameGeneration(true)).toBe(false);
    expect(shouldAutoEnableCaptureFixForFrameGeneration(false)).toBe(false);
  });

  it('returns the redesigned display health messages', () => {
    expect(frameGenDisplayHealthMessage(true, 'game-provided')).toBe(
      'DLSS/FSR frame generation should use virtual display for reliable, smooth capture.',
    );
    expect(frameGenDisplayHealthMessage(false, 'game-provided')).toBe(
      'Frame generation on a physical display may micro-stutter or judder without suitable frame pacing. Use a virtual display for low-latency, smooth capture.',
    );
    expect(frameGenDisplayHealthMessage(false, 'lossless-scaling')).toBe(
      'Frame generation on a physical display may micro-stutter or judder without suitable frame pacing. Use a virtual display for low-latency, smooth capture.',
    );
    expect(frameGenDisplayHealthMessage(false, 'nvidia-smooth-motion')).toBe(
      'Frame generation on a physical display may micro-stutter or judder without suitable frame pacing. Use a virtual display for low-latency, smooth capture.',
    );
    expect(frameGenDisplayHealthMessage(false, 'off')).toBe(
      'Frame generation on a physical display may micro-stutter or judder without suitable frame pacing. Use a virtual display for low-latency, smooth capture.',
    );
  });
});
