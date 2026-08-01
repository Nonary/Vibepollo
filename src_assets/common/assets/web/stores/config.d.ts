export interface MetaInfo {
  platform?: string;
  status?: boolean;
  version?: string;
  commit?: string;
  branch?: string;
  release_date?: string;
  gpus?: Array<{
    description?: string;
    pnp_id?: string;
    vendor_id?: number | string;
    device_id?: number | string;
    dedicated_video_memory?: number | string;
  }>;
  has_nvidia_gpu?: boolean;
  has_amd_gpu?: boolean;
  has_intel_gpu?: boolean;
  windows_display_version?: string;
  windows_release_id?: string;
  windows_product_name?: string;
  windows_current_build?: string;
  windows_build_number?: number;
  windows_major_version?: number;
  windows_minor_version?: number;
}

export type ConfigState = { platform: string } & Record<string, any>;
type ConfigSavingState = 'idle' | 'dirty' | 'saving' | 'saved' | 'error';

interface ConfigSaveResult {
  appliedNow: boolean;
  deferred: boolean;
  restartRequired: boolean;
}

interface ConfigStore {
  tabs: readonly unknown[];
  defaults: Record<string, unknown>;
  config: ConfigState;
  version: number;
  manualDirty: boolean;
  savingState: ConfigSavingState;
  metadata: MetaInfo | null;
  isWindows10Host: boolean;
  loading: boolean;
  error: string | null;
  validationError: string | null;
  fetchConfig(force?: boolean): Promise<ConfigState | null>;
  setConfig(value: unknown): void;
  updateOption(key: string, value: unknown): void;
  setAdapterPreference(name: string, pnpId: string): void;
  markManualDirty(key?: string): void;
  resetManualDirty(): void;
  save(): Promise<boolean>;
  serialize(): Record<string, unknown> | null;
  flushPatchQueue(): Promise<boolean>;
  startAutosave(): void;
  stopAutosave(): void;
  reloadConfig(): Promise<ConfigState | null>;
  hasPendingPatch(): boolean;
  autosaveIntervalMs: number;
  nextAutosaveAt(): number;
  lastSaveResult: ConfigSaveResult | null;
}

export declare const useConfigStore: () => ConfigStore;
