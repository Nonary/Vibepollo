export interface PrepCmd {
  do?: string;
  undo?: string;
  elevated?: boolean;
}

export interface App {
  name?: string;
  output?: string;
  'display-output'?: string;
  cmd?: string | string[];
  uuid?: string;
  'working-dir'?: string;
  'image-path'?: string;
  'playnite-icon-path'?: string;
  'image-version'?: number;
  'playnite-icon-version'?: number;
  'exclude-global-prep-cmd'?: boolean;
  'config-overrides'?: Record<string, unknown>;
  elevated?: boolean;
  'auto-detach'?: boolean;
  'wait-all'?: boolean;
  'frame-gen-limiter-fix'?: boolean;
  'gen1-framegen-fix'?: boolean;
  'gen2-framegen-fix'?: boolean;
  'exit-timeout'?: number;
  'prep-cmd'?: PrepCmd[];
  detached?: string[];
  'lossless-scaling-enabled'?: boolean;
  'lossless-scaling-framegen'?: boolean;
  'lossless-scaling-target-fps'?: number;
  'lossless-scaling-rtss-limit'?: number;
  'lossless-scaling-profile'?: string;
  'lossless-scaling-recommended'?: Record<string, unknown>;
  'lossless-scaling-custom'?: Record<string, unknown>;
  'lossless-scaling-launch-delay'?: number;
  [key: string]: any;
}

interface AppsStore {
  apps: App[];
  setApps(list: App[]): void;
  loadApps(force?: boolean): Promise<App[]>;
}

export declare const useAppsStore: () => AppsStore;
