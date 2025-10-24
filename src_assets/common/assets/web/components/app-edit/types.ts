export interface PrepCmd {
  do: string;
  undo: string;
  elevated?: boolean;
}

export type LosslessProfileKey = 'recommended' | 'custom';

export type LosslessScalingMode =
  | 'off'
  | 'ls1'
  | 'fsr'
  | 'nis'
  | 'sgsr'
  | 'bcas'
  | 'anime4k'
  | 'xbr'
  | 'sharp-bilinear'
  | 'integer'
  | 'nearest';

export type Anime4kSize = 'S' | 'M' | 'L' | 'VL' | 'UL';

export type FrameGenerationProvider = 'lossless-scaling' | 'nvidia-smooth-motion';
export type FrameGenerationMode = 'off' | FrameGenerationProvider;

export interface LosslessProfileOverrides {
  performanceMode: boolean | null;
  flowScale: number | null;
  resolutionScale: number | null;
  scalingMode: LosslessScalingMode | null;
  sharpening: number | null;
  anime4kSize: Anime4kSize | null;
  anime4kVrs: boolean | null;
}

export interface LosslessProfileDefaults {
  performanceMode: boolean;
  flowScale: number;
  resolutionScale: number;
  scalingMode: LosslessScalingMode;
  sharpening: number;
  anime4kSize: Anime4kSize;
  anime4kVrs: boolean;
}

export interface AppForm {
  index: number;
  name: string;
  output: string;
  cmd: string;
  workingDir: string;
  imagePath: string;
  excludeGlobalPrepCmd: boolean;
  elevated: boolean;
  autoDetach: boolean;
  waitAll: boolean;
  frameGenLimiterFix: boolean;
  exitTimeout: number;
  prepCmd: PrepCmd[];
  detached: string[];
  virtualScreen: boolean;
  gen1FramegenFix: boolean;
  gen2FramegenFix: boolean;
  frameGenerationProvider: FrameGenerationProvider;
  losslessScalingEnabled: boolean;
  losslessScalingTargetFps: number | null;
  losslessScalingRtssLimit: number | null;
  losslessScalingRtssTouched: boolean;
  losslessScalingProfile: LosslessProfileKey;
  losslessScalingProfiles: Record<LosslessProfileKey, LosslessProfileOverrides>;
  playniteId?: string | undefined;
  playniteManaged?: 'manual' | string | undefined;
}

export interface ServerApp {
  name?: string;
  output?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'exclude-global-prep-cmd'?: boolean;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'frame-gen-limiter-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: Array<{ do?: string; undo?: string; elevated?: boolean }>;
  detached?: string[];
  'virtual-screen'?: boolean;
  'playnite-id'?: string | undefined;
  'playnite-managed'?: 'manual' | string | undefined;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'dlss-framegen-capture-fix'?: boolean;
  'frame-generation-provider'?: string;
  'lossless-scaling-framegen'?: boolean;
  'lossless-scaling-target-fps'?: number | string | null;
  'lossless-scaling-rtss-limit'?: number | string | null;
  'lossless-scaling-profile'?: string;
  'lossless-scaling-recommended'?: Record<string, unknown>;
  'lossless-scaling-custom'?: Record<string, unknown>;
}

export type FrameGenRequirementStatus = 'pass' | 'warn' | 'fail' | 'unknown';

export interface FrameGenDisplayTarget {
  fps: number;
  requiredHz: number;
  supported: boolean | null;
}

export interface FrameGenHealth {
  checkedAt: number;
  capture: {
    status: FrameGenRequirementStatus;
    method: string;
    message: string;
  };
  rtss: {
    status: FrameGenRequirementStatus;
    installed: boolean;
    running: boolean;
    hooksDetected: boolean;
    message: string;
  };
  display: {
    status: FrameGenRequirementStatus;
    deviceLabel: string;
    deviceId: string;
    currentHz: number | null;
    targets: FrameGenDisplayTarget[];
    virtualActive: boolean;
    message: string;
    error?: string | null;
  };
  suggestion?: {
    message: string;
    emphasis: 'info' | 'warning';
  };
}
