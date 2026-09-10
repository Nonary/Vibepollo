import type {
  Anime4kSize,
  FrameGenerationMode,
  FrameGenerationProvider,
  LosslessProfileDefaults,
  LosslessProfileKey,
  LosslessProfileOverrides,
  LosslessScalingMode,
} from './types';

export const LOSSLESS_FLOW_MIN = 0;
export const LOSSLESS_FLOW_MAX = 100;
export const LOSSLESS_RESOLUTION_MIN = 10;
export const LOSSLESS_RESOLUTION_MAX = 100;
export const LOSSLESS_SHARPNESS_MIN = 1;
export const LOSSLESS_SHARPNESS_MAX = 10;

export type LocalizedOption<T> = { label?: string; labelKey?: string; value: T };

export const LOSSLESS_SCALING_OPTIONS: LocalizedOption<LosslessScalingMode>[] = [
  { labelKey: '_common.disabled', value: 'off' },
  { label: 'LS1', value: 'ls1' },
  { label: 'FSR 1.0', value: 'fsr' },
  { label: 'NIS', value: 'nis' },
  { label: 'SGSR', value: 'sgsr' },
  { labelKey: 'apps.lossless.scaling_bcas', value: 'bcas' },
  { label: 'Anime4K', value: 'anime4k' },
  { label: 'xBR', value: 'xbr' },
  { labelKey: 'apps.lossless.scaling_sharp_bilinear', value: 'sharp-bilinear' },
  { labelKey: 'apps.lossless.scaling_integer', value: 'integer' },
  { labelKey: 'apps.lossless.scaling_nearest', value: 'nearest' },
];

export const LOSSLESS_SCALING_SHARPENING = new Set<LosslessScalingMode>([
  'ls1',
  'fsr',
  'nis',
  'sgsr',
]);

export const LOSSLESS_ANIME_SIZES: LocalizedOption<Anime4kSize>[] = [
  { labelKey: 'apps.lossless.anime_size_s', value: 'S' },
  { labelKey: 'apps.lossless.anime_size_m', value: 'M' },
  { labelKey: 'apps.lossless.anime_size_l', value: 'L' },
  { labelKey: 'apps.lossless.anime_size_vl', value: 'VL' },
  { labelKey: 'apps.lossless.anime_size_ul', value: 'UL' },
];

export const FRAME_GENERATION_PROVIDERS: LocalizedOption<FrameGenerationProvider>[] = [
  { labelKey: 'apps.framegen.provider_game_provided', value: 'game-provided' },
  { label: 'Lossless Scaling', value: 'lossless-scaling' },
  { label: 'NVIDIA Smooth Motion', value: 'nvidia-smooth-motion' },
];

export const LOSSLESS_PROFILE_DEFAULTS: Record<LosslessProfileKey, LosslessProfileDefaults> = {
  recommended: {
    performanceMode: true,
    flowScale: 50,
    resolutionScale: 100,
    scalingMode: 'off',
    sharpening: 5,
    anime4kSize: 'M',
    anime4kVrs: false,
  },
  custom: {
    performanceMode: false,
    flowScale: 50,
    resolutionScale: 100,
    scalingMode: 'off',
    sharpening: 5,
    anime4kSize: 'S',
    anime4kVrs: false,
  },
};

export function emptyLosslessOverrides(): LosslessProfileOverrides {
  return {
    performanceMode: null,
    flowScale: null,
    resolutionScale: null,
    scalingMode: null,
    sharpening: null,
    anime4kSize: null,
    anime4kVrs: null,
  };
}

export function emptyLosslessProfileState(): Record<LosslessProfileKey, LosslessProfileOverrides> {
  return {
    recommended: emptyLosslessOverrides(),
    custom: emptyLosslessOverrides(),
  };
}

export function normalizeFrameGenerationProvider(value: unknown): FrameGenerationProvider {
  if (typeof value !== 'string') return 'lossless-scaling';
  const compact = value
    .toLowerCase()
    .split('')
    .filter((ch) => /[a-z0-9]/.test(ch))
    .join('');
  if (compact === 'nvidiasmoothmotion' || compact === 'smoothmotion' || compact === 'nvidia') {
    return 'nvidia-smooth-motion';
  }
  if (compact === 'gameprovided' || compact === 'game') return 'game-provided';
  if (compact === 'losslessscaling' || compact === 'lossless') return 'lossless-scaling';
  return 'lossless-scaling';
}

export function parseFrameGenerationMode(value: unknown): FrameGenerationMode | null {
  if (typeof value !== 'string') return null;
  const compact = value
    .toLowerCase()
    .split('')
    .filter((ch) => /[a-z0-9]/.test(ch))
    .join('');
  if (compact === 'off' || compact === 'none' || compact === 'disabled') return 'off';
  if (compact === 'nvidiasmoothmotion' || compact === 'smoothmotion' || compact === 'nvidia') {
    return 'nvidia-smooth-motion';
  }
  if (compact === 'gameprovided' || compact === 'game') return 'game-provided';
  if (compact === 'losslessscaling' || compact === 'lossless') return 'lossless-scaling';
  return null;
}

export function parseNumeric(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) return value;
  if (typeof value !== 'string') return null;
  const trimmed = value.trim();
  if (!trimmed) return null;
  const parsed = Number(trimmed);
  return Number.isFinite(parsed) ? parsed : null;
}

export function clampFlow(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  return Math.min(LOSSLESS_FLOW_MAX, Math.max(LOSSLESS_FLOW_MIN, Math.round(value)));
}

export function clampResolution(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  return Math.min(LOSSLESS_RESOLUTION_MAX, Math.max(LOSSLESS_RESOLUTION_MIN, Math.round(value)));
}

export function clampSharpness(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  return Math.min(LOSSLESS_SHARPNESS_MAX, Math.max(LOSSLESS_SHARPNESS_MIN, Math.round(value)));
}

export function defaultRtssFromTarget(target: number | null): number | null {
  if (typeof target !== 'number' || !Number.isFinite(target) || target <= 0) return null;
  return Math.min(360, Math.max(1, Math.round(target / 2)));
}

export function parseLosslessProfileKey(value: unknown): LosslessProfileKey {
  if (typeof value === 'string' && value.toLowerCase() === 'custom') return 'custom';
  return 'recommended';
}

export function parseLosslessOverrides(input: unknown): LosslessProfileOverrides {
  const overrides = emptyLosslessOverrides();
  if (!input || typeof input !== 'object' || Array.isArray(input)) return overrides;
  const source = input as Record<string, unknown>;
  if (typeof source['performance-mode'] === 'boolean') {
    overrides.performanceMode = source['performance-mode'];
  }
  const flow = clampFlow(parseNumeric(source['flow-scale']));
  if (flow !== null) overrides.flowScale = flow;
  const resolution = clampResolution(parseNumeric(source['resolution-scale']));
  if (resolution !== null) overrides.resolutionScale = resolution;
  const mode =
    typeof source['scaling-type'] === 'string' ? source['scaling-type'].toLowerCase() : '';
  if (LOSSLESS_SCALING_OPTIONS.some((option) => option.value === mode)) {
    overrides.scalingMode = mode as LosslessScalingMode;
  }
  const sharpening = clampSharpness(parseNumeric(source.sharpening));
  if (sharpening !== null) overrides.sharpening = sharpening;
  const animeSize =
    typeof source['anime4k-size'] === 'string' ? source['anime4k-size'].toUpperCase() : '';
  if (LOSSLESS_ANIME_SIZES.some((option) => option.value === animeSize)) {
    overrides.anime4kSize = animeSize as Anime4kSize;
  }
  if (typeof source['anime4k-vrs'] === 'boolean') overrides.anime4kVrs = source['anime4k-vrs'];
  return overrides;
}
