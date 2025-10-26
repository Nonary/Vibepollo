import type {
  Anime4kSize,
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

export const LOSSLESS_SCALING_OPTIONS: { label: string; value: LosslessScalingMode }[] = [
  { label: 'Off', value: 'off' },
  { label: 'LS1', value: 'ls1' },
  { label: 'FSR 1.0', value: 'fsr' },
  { label: 'NIS', value: 'nis' },
  { label: 'SGSR', value: 'sgsr' },
  { label: 'BCAS (Anime)', value: 'bcas' },
  { label: 'Anime4K', value: 'anime4k' },
  { label: 'xBR', value: 'xbr' },
  { label: 'Sharp Bilinear', value: 'sharp-bilinear' },
  { label: 'Integer Scale', value: 'integer' },
  { label: 'Nearest Neighbour', value: 'nearest' },
];

export const LOSSLESS_SCALING_SHARPENING = new Set<LosslessScalingMode>([
  'ls1',
  'fsr',
  'nis',
  'sgsr',
]);

export const LOSSLESS_ANIME_SIZES: { label: string; value: Anime4kSize }[] = [
  { label: 'Small', value: 'S' },
  { label: 'Medium', value: 'M' },
  { label: 'Large', value: 'L' },
  { label: 'Very Large', value: 'VL' },
  { label: 'Ultra Large', value: 'UL' },
];

export const FRAME_GENERATION_PROVIDERS: Array<{ label: string; value: FrameGenerationProvider }> =
  [
    { label: 'Game Provided', value: 'game-provided' },
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
  if (typeof value !== 'string') {
    return 'lossless-scaling';
  }
  const compact = value
    .toLowerCase()
    .split('')
    .filter((ch) => /[a-z0-9]/.test(ch))
    .join('');
  if (compact === 'nvidiasmoothmotion' || compact === 'smoothmotion' || compact === 'nvidia') {
    return 'nvidia-smooth-motion';
  }
  if (compact === 'gameprovided' || compact === 'game') {
    return 'game-provided';
  }
  if (compact === 'losslessscaling' || compact === 'lossless') {
    return 'lossless-scaling';
  }
  return 'lossless-scaling';
}

export function parseNumeric(value: unknown): number | null {
  if (typeof value === 'number' && Number.isFinite(value)) {
    return value;
  }
  if (typeof value === 'string') {
    const trimmed = value.trim();
    if (trimmed.length === 0) return null;
    const parsed = Number(trimmed);
    if (Number.isFinite(parsed)) {
      return parsed;
    }
  }
  return null;
}

export function clampFlow(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_FLOW_MAX, Math.max(LOSSLESS_FLOW_MIN, rounded));
}

export function clampResolution(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_RESOLUTION_MAX, Math.max(LOSSLESS_RESOLUTION_MIN, rounded));
}

export function clampSharpness(value: number | null): number | null {
  if (typeof value !== 'number' || !Number.isFinite(value)) return null;
  const rounded = Math.round(value);
  return Math.min(LOSSLESS_SHARPNESS_MAX, Math.max(LOSSLESS_SHARPNESS_MIN, rounded));
}

export function defaultRtssFromTarget(target: number | null): number | null {
  if (typeof target !== 'number' || !Number.isFinite(target) || target <= 0) {
    return null;
  }
  return Math.min(360, Math.max(1, Math.round(target / 2)));
}

export function parseLosslessProfileKey(value: unknown): LosslessProfileKey {
  if (typeof value === 'string') {
    const normalized = value.toLowerCase();
    if (normalized === 'custom') {
      return 'custom';
    }
    if (normalized === 'recommended') {
      return 'recommended';
    }
  }
  return 'recommended';
}

export function parseLosslessOverrides(input: unknown): LosslessProfileOverrides {
  const overrides = emptyLosslessOverrides();
  if (!input || typeof input !== 'object') {
    return overrides;
  }
  const source = input as Record<string, unknown>;
  if (typeof source['performance-mode'] === 'boolean') {
    overrides.performanceMode = source['performance-mode'] as boolean;
  }
  const rawFlow = clampFlow(parseNumeric(source['flow-scale']));
  if (rawFlow !== null) {
    overrides.flowScale = rawFlow;
  }
  const rawResolution = clampResolution(parseNumeric(source['resolution-scale']));
  if (rawResolution !== null) {
    overrides.resolutionScale = rawResolution;
  }
  const modeRaw = typeof source['scaling-type'] === 'string' ? source['scaling-type'] : null;
  if (modeRaw) {
    const normalized = modeRaw.toLowerCase() as LosslessScalingMode;
    if (LOSSLESS_SCALING_OPTIONS.some((o) => o.value === normalized)) {
      overrides.scalingMode = normalized;
    }
  }
  const rawSharpness = clampSharpness(parseNumeric(source['sharpening']));
  if (rawSharpness !== null) {
    overrides.sharpening = rawSharpness;
  }
  const animeSizeRaw =
    typeof source['anime4k-size'] === 'string' ? source['anime4k-size'].toUpperCase() : null;
  if (animeSizeRaw && LOSSLESS_ANIME_SIZES.some((o) => o.value === animeSizeRaw)) {
    overrides.anime4kSize = animeSizeRaw as Anime4kSize;
  }
  if (typeof source['anime4k-vrs'] === 'boolean') {
    overrides.anime4kVrs = source['anime4k-vrs'] as boolean;
  }
  return overrides;
}
