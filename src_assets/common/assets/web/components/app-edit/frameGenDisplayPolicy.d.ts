import type { AppVirtualDisplayMode, FrameGenerationMode } from './types';

export type DisplaySelection = 'global' | 'virtual' | 'physical';
export declare const VIRTUAL_DISPLAY_SELECTION: string;

export interface FrameGenDisplayResolutionInput {
  displaySelection: DisplaySelection;
  appVirtualDisplayMode: AppVirtualDisplayMode | null;
  globalVirtualDisplayMode: AppVirtualDisplayMode;
  globalOutputName: string;
}

export interface FrameGenDisplayNotice {
  type: 'info' | 'warning';
  key: string;
}

export declare function resolvesToVirtualDisplay(input: FrameGenDisplayResolutionInput): boolean;
export declare function physicalFrameGenDisplayWarningKey(): string;
export declare function frameGenDisplayNotice(
  usesVirtualDisplay: boolean,
  mode: FrameGenerationMode,
): FrameGenDisplayNotice | null;
export declare function frameGenDisplayHealthKey(
  usesVirtualDisplay: boolean,
  mode?: FrameGenerationMode,
): string;
