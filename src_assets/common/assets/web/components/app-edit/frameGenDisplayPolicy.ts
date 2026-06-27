import type { AppVirtualDisplayMode } from './types';

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

export function frameGenDisplayHealthMessage(usesVirtualDisplay: boolean): string {
  return usesVirtualDisplay
    ? '4x refresh + Reflex path.'
    : '1x refresh; not recommended for frame generation; use virtual display for best pacing.';
}
