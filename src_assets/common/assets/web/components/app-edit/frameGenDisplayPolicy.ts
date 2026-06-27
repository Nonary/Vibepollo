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
  void captureFixEnabled;
  void usesVirtualDisplay;
  return false;
}

export function shouldAutoEnableCaptureFixForFrameGeneration(usesVirtualDisplay: boolean): boolean {
  void usesVirtualDisplay;
  return false;
}

export function physicalFrameGenDisplayWarning(rtssInstalled?: boolean | null): string {
  if (rtssInstalled === true) {
    return 'Frame generation on a physical display can add latency with RTSS frame limiting. Use a virtual display for low-latency, smooth capture.';
  }

  return 'Frame generation on a physical display may micro-stutter or judder without suitable frame pacing. Use a virtual display for low-latency, smooth capture.';
}

export function physicalGameFrameGenCaptureWarning(rtssInstalled?: boolean | null): string {
  return physicalFrameGenDisplayWarning(rtssInstalled);
}

export function frameGenDisplayHealthMessage(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode = 'off',
): string {
  if (usesVirtualDisplay) {
    if (mode === 'game-provided') {
      return 'DLSS/FSR frame generation should use virtual display for reliable, smooth capture.';
    }
    return 'Virtual display uses automatic frame pacing for smoother frame generation.';
  }
  void mode;
  return physicalFrameGenDisplayWarning();
}
