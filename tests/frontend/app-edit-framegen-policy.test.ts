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

  it('keeps capture fix available for physical-display apps', () => {
    const usesVirtual = resolvesToVirtualDisplay({
      displaySelection: 'physical',
      appVirtualDisplayMode: 'disabled',
      globalVirtualDisplayMode: 'per_client',
      globalOutputName: '',
    });

    expect(usesVirtual).toBe(false);
    expect(shouldPersistFrameGenerationCaptureFix(true, usesVirtual)).toBe(true);
  });

  it('does not auto-enable capture fix for virtual-display frame generation', () => {
    expect(shouldAutoEnableCaptureFixForFrameGeneration(true)).toBe(false);
    expect(shouldAutoEnableCaptureFixForFrameGeneration(false)).toBe(true);
  });

  it('returns the redesigned display health messages', () => {
    expect(frameGenDisplayHealthMessage(true, 'game-provided')).toBe(
      'Virtual display uses 4x refresh with automatic frame pacing.',
    );
    expect(frameGenDisplayHealthMessage(false, 'game-provided')).toBe(
      'Physical display is not recommended for game-provided DLSS/FSR capture. Use the virtual display for the 4x pacing path.',
    );
    expect(frameGenDisplayHealthMessage(false, 'lossless-scaling')).toBe(
      'Physical display is supported for Lossless Scaling frame generation. Use enough refresh headroom for the generated output.',
    );
    expect(frameGenDisplayHealthMessage(false, 'nvidia-smooth-motion')).toBe(
      'Physical display is supported for NVIDIA Smooth Motion. Use enough refresh headroom for the generated output.',
    );
  });
});
