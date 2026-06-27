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

// Single source of truth for the physical-display frame-generation warning.
// Mirrors config.physical_display_framegen_warning shown on the Audio/Video tab so
// both screens tell users the same thing about capture limits and latency.
export function physicalFrameGenDisplayWarning(): string {
  return 'Physical displays cannot capture DLSS/FSR generated frames. If you use DLSS/FSR frame generation with an RTSS frame limiter on a physical screen, latency can increase by 50 ms or more. Without suitable frame pacing, it may micro-stutter or judder. Use a virtual display for low-latency, smooth capture.';
}

function virtualFrameGenDisplayMessage(mode: FrameGenerationMode): string {
  if (mode === 'game-provided') {
    return 'Virtual display captures DLSS/FSR generated frames reliably and paces them automatically.';
  }
  return 'Virtual display uses automatic frame pacing for smoother frame generation.';
}

export interface FrameGenDisplayNotice {
  type: 'info' | 'warning';
  message: string;
}

// Resolves the banner shown in the app editor's Frame Generation section.
// Returns null when no frame generation is selected so we never warn about a
// feature the app is not using.
export function frameGenDisplayNotice(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode,
): FrameGenDisplayNotice | null {
  if (mode === 'off') {
    return null;
  }
  if (usesVirtualDisplay) {
    return { type: 'info', message: virtualFrameGenDisplayMessage(mode) };
  }
  return { type: 'warning', message: physicalFrameGenDisplayWarning() };
}

export function frameGenDisplayHealthMessage(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode = 'off',
): string {
  if (usesVirtualDisplay) {
    return virtualFrameGenDisplayMessage(mode);
  }
  return physicalFrameGenDisplayWarning();
}
