import type {
  Anime4kSize,
  FrameGenerationMode,
  FrameGenerationProvider,
  LosslessProfileDefaults,
  LosslessProfileKey,
  LosslessProfileOverrides,
  LosslessScalingMode,
} from './types';

export declare const LOSSLESS_FLOW_MIN: number;
export declare const LOSSLESS_FLOW_MAX: number;
export declare const LOSSLESS_RESOLUTION_MIN: number;
export declare const LOSSLESS_RESOLUTION_MAX: number;
export declare const LOSSLESS_SHARPNESS_MIN: number;
export declare const LOSSLESS_SHARPNESS_MAX: number;

export type LocalizedOption<T> = { label?: string; labelKey?: string; value: T };

export declare const LOSSLESS_SCALING_OPTIONS: LocalizedOption<LosslessScalingMode>[];
export declare const LOSSLESS_SCALING_SHARPENING: Set<LosslessScalingMode>;
export declare const LOSSLESS_ANIME_SIZES: LocalizedOption<Anime4kSize>[];
export declare const FRAME_GENERATION_PROVIDERS: LocalizedOption<FrameGenerationProvider>[];
export declare const LOSSLESS_PROFILE_DEFAULTS: Record<LosslessProfileKey, LosslessProfileDefaults>;

export declare function emptyLosslessOverrides(): LosslessProfileOverrides;
export declare function emptyLosslessProfileState(): Record<
  LosslessProfileKey,
  LosslessProfileOverrides
>;
export declare function normalizeFrameGenerationProvider(value: unknown): FrameGenerationProvider;
export declare function parseFrameGenerationMode(value: unknown): FrameGenerationMode | null;
export declare function parseNumeric(value: unknown): number | null;
export declare function clampFlow(value: number | null): number | null;
export declare function clampResolution(value: number | null): number | null;
export declare function clampSharpness(value: number | null): number | null;
export declare function defaultRtssFromTarget(target: number | null): number | null;
export declare function parseLosslessProfileKey(value: unknown): LosslessProfileKey;
export declare function parseLosslessOverrides(input: unknown): LosslessProfileOverrides;
