import type { AppVirtualDisplayMode, FrameGenerationMode } from './types';

export type DisplaySelection = 'global' | 'virtual' | 'physical';

export const VIRTUAL_DISPLAY_SELECTION = 'sunshine:virtual_display';

export interface FrameGenDisplayResolutionInput {
  displaySelection: DisplaySelection;
  appVirtualDisplayMode: AppVirtualDisplayMode | null;
  globalVirtualDisplayMode: AppVirtualDisplayMode;
  globalOutputName: string;
}

export function resolvesToVirtualDisplay(input: FrameGenDisplayResolutionInput): boolean {
  if (input.displaySelection === 'virtual') {
    return true;
  }
  if (input.displaySelection === 'physical') {
    return false;
  }
  const appMode = input.appVirtualDisplayMode;
  const mode = appMode ?? input.globalVirtualDisplayMode;
  if (mode === 'per_client' || mode === 'shared') {
    return true;
  }
  return input.globalOutputName === VIRTUAL_DISPLAY_SELECTION;
}

export function shouldPersistFrameGenerationCaptureFix(
  captureFixEnabled: boolean,
  usesVirtualDisplay: boolean,
): boolean {
  return captureFixEnabled && !usesVirtualDisplay;
}

export function shouldAutoEnableCaptureFixForFrameGeneration(usesVirtualDisplay: boolean): boolean {
  return !usesVirtualDisplay;
}

export function frameGenDisplayHealthMessage(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode = 'off',
): string {
  if (usesVirtualDisplay) {
    return 'Virtual display uses 4x refresh with automatic frame pacing.';
  }
  if (mode === 'game-provided') {
    return 'Physical display is not recommended for game-provided DLSS/FSR capture. Use the virtual display for the 4x pacing path.';
  }
  if (mode === 'lossless-scaling') {
    return 'Physical display is supported for Lossless Scaling frame generation. Use enough refresh headroom for the generated output.';
  }
  if (mode === 'nvidia-smooth-motion') {
    return 'Physical display is supported for NVIDIA Smooth Motion. Use enough refresh headroom for the generated output.';
  }
  return 'Physical display is supported for external frame generation. Use enough refresh headroom for the generated output.';
}
