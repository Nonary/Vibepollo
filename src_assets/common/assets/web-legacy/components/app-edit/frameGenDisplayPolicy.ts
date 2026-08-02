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

// These helpers return i18n message KEYS (not literal text) so this pure module
// stays translation-agnostic; callers resolve them with their i18n `t()`.

// Single source of truth for the physical-display frame-generation warning.
// Mirrors the Audio/Video tab so both screens tell users the same thing about
// capture limits and latency.
export function physicalFrameGenDisplayWarningKey(): string {
  return 'config.physical_display_framegen_warning';
}

function virtualFrameGenDisplayKey(mode: FrameGenerationMode): string {
  return mode === 'game-provided'
    ? 'apps.framegen.notice_virtual_game_provided'
    : 'apps.framegen.notice_virtual_generic';
}

export interface FrameGenDisplayNotice {
  type: 'info' | 'warning';
  key: string;
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
    return { type: 'info', key: virtualFrameGenDisplayKey(mode) };
  }
  return { type: 'warning', key: physicalFrameGenDisplayWarningKey() };
}

export function frameGenDisplayHealthKey(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode = 'off',
): string {
  if (usesVirtualDisplay) {
    return virtualFrameGenDisplayKey(mode);
  }
  return physicalFrameGenDisplayWarningKey();
}
