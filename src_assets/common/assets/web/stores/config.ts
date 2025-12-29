import { defineStore } from 'pinia';
import { ref } from 'vue';
import { http } from '@/http';

// Metadata describing build/runtime info returned by /api/meta
export interface MetaInfo {
  platform?: string;
  status?: boolean;
  version?: string;
  commit?: string;
  branch?: string;
  release_date?: string; // ISO 8601 when available
  gpus?: Array<{
    description?: string;
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

// --- Defaults (flat) -------------------------------------------------------
// Keep these separate from runtime state so reading defaults does NOT mutate
// the actual config object that will be POSTed back to the server.

type UnionToIntersection<U> = (U extends unknown ? (arg: U) => void : never) extends (
  arg: infer I,
) => void
  ? I
  : never;

type Mutable<T> = { -readonly [K in keyof T]: T[K] };

type WidenLiteral<T> = T extends string
  ? string
  : T extends number
    ? number
    : T extends boolean
      ? boolean
      : T extends null
        ? null
        : T extends undefined
          ? undefined
          : T extends ReadonlyArray<infer U>
            ? Array<WidenLiteral<U>>
            : T extends Record<string, unknown>
              ? { [K in keyof Mutable<T>]: WidenLiteral<Mutable<T>[K]> }
              : T;
const defaultGroups = [
  {
    id: 'general',
    name: 'General',
    options: {
      locale: 'en',
      sunshine_name: '',
      min_log_level: 2,
      global_prep_cmd: [] as Array<{ do: string; undo: string; elevated?: boolean }>,
      notify_pre_releases: 'disabled',
      update_check_interval: 86400,
      session_token_ttl_seconds: 86400,
      remember_me_refresh_token_ttl_seconds: 604800,
      system_tray: true,
    },
  },
  {
    id: 'input',
    name: 'Input',
    options: {
      controller: 'enabled',
      gamepad: 'auto',
      ds4_back_as_touchpad_click: 'enabled',
      motion_as_ds4: 'enabled',
      touchpad_as_ds4: 'enabled',
      back_button_timeout: -1,
      keyboard: 'enabled',
      key_repeat_delay: 500,
      key_repeat_frequency: 24.9,
      always_send_scancodes: 'enabled',
      key_rightalt_to_key_win: 'disabled',
      mouse: 'enabled',
      high_resolution_scrolling: 'enabled',
      native_pen_touch: 'enabled',
      keybindings: '[0x10,0xA0,0x11,0xA2,0x12,0xA4]',
      ds5_inputtino_randomize_mac: true,
    },
  },
  {
    id: 'av',
    name: 'Audio/Video',
    options: {
      audio_sink: '',
      virtual_sink: '',
      install_steam_audio_drivers: 'enabled',
      stream_audio: 'enabled',
      adapter_name: '',
      output_name: '',
      virtual_display_mode: 'disabled',
      virtual_display_layout: 'exclusive',
      dd_configuration_option: 'verify_only',
      dd_resolution_option: 'auto',
      dd_manual_resolution: '',
      dd_refresh_rate_option: 'auto',
      dd_manual_refresh_rate: '',
      dd_hdr_option: 'auto',
      dd_hdr_request_override: 'auto',
      dd_config_revert_delay: 3000,
      dd_config_revert_on_disconnect: 'disabled',
      dd_always_restore_from_golden: false,
      dd_snapshot_exclude_devices: [] as Array<string>,
      dd_snapshot_restore_hotkey: '',
      dd_snapshot_restore_hotkey_modifiers: 'ctrl+alt+shift',
      dd_activate_virtual_display: false,
      dd_mode_remapping: {
        mixed: [] as Array<Record<string, string>>,
        resolution_only: [] as Array<Record<string, string>>,
        refresh_rate_only: [] as Array<Record<string, string>>,
      },
      dd_wa_virtual_double_refresh: true,
      dd_wa_dummy_plug_hdr10: false,
      max_bitrate: 0,
      minimum_fps_target: 20,
      lossless_scaling_path: '',
    },
  },
  {
    id: 'network',
    name: 'Network',
    options: {
      upnp: 'disabled',
      address_family: 'ipv4',
      port: 47989,
      origin_web_ui_allowed: 'lan',
      external_ip: '',
      lan_encryption_mode: 0,
      wan_encryption_mode: 1,
      ping_timeout: 10000,
    },
  },
  {
    id: 'files',
    name: 'Config Files',
    options: {
      file_apps: '',
      credentials_file: '',
      log_path: '',
      pkey: '',
      cert: '',
      file_state: '',
      vibeshine_file_state: '',
    },
  },
  {
    id: 'playnite',
    name: 'Playnite',
    options: {
      playnite_auto_sync: true,
      playnite_sync_all_installed: false,
      playnite_recent_games: 10,
      playnite_recent_max_age_days: 0,
      playnite_autosync_delete_after_days: 0,
      playnite_autosync_require_replacement: true,
      playnite_autosync_remove_uninstalled: true,
      playnite_focus_attempts: 3,
      playnite_focus_timeout_secs: 15,
      playnite_focus_exit_on_first: false,
      playnite_fullscreen_entry_enabled: false,
      playnite_sync_categories: [] as Array<{ id: string; name: string }>,
      playnite_sync_plugins: [] as Array<{ id: string; name: string }>,
      playnite_exclude_categories: [] as Array<{ id: string; name: string }>,
      playnite_exclude_plugins: [] as Array<{ id: string; name: string }>,
      playnite_exclude_games: [] as Array<{ id: string; name: string }>,
      playnite_install_dir: '',
      playnite_extensions_dir: '',
    },
  },
  {
    id: 'advanced',
    name: 'Advanced',
    options: {
      fec_percentage: 20,
      qp: 28,
      min_threads: 2,
      hevc_mode: 0,
      av1_mode: 0,
      prefer_10bit_sdr: false,
      capture: '',
      encoder: '',
    },
  },
  {
    id: 'rtss',
    name: 'Frame Limiter',
    options: {
      frame_limiter_enable: false,
      frame_limiter_provider: 'auto',
      frame_limiter_fps_limit: 0,
      rtss_install_path: '',
      rtss_frame_limit_type: 'async',
      frame_limiter_disable_vsync: false,
    },
  },
  {
    id: 'nv',
    name: 'NVIDIA NVENC Encoder',
    options: {
      nvenc_preset: 4,
      nvenc_twopass: 'quarter_res',
      nvenc_spatial_aq: 'disabled',
      nvenc_vbv_increase: 0,
      nvenc_realtime_hags: 'enabled',
      nvenc_latency_over_power: 'enabled',
      nvenc_opengl_vulkan_on_dxgi: 'enabled',
      nvenc_h264_cavlc: 'disabled',
    },
  },
  {
    id: 'qsv',
    name: 'Intel QuickSync Encoder',
    options: {
      qsv_preset: 'medium',
      qsv_coder: 'auto',
      qsv_slow_hevc: 'disabled',
    },
  },
  {
    id: 'amd',
    name: 'AMD AMF Encoder',
    options: {
      amd_usage: 'ultralowlatency',
      amd_rc: 'vbr_latency',
      amd_enforce_hrd: 'disabled',
      amd_quality: 'balanced',
      amd_preanalysis: 'disabled',
      amd_vbaq: 'enabled',
      amd_coder: 'auto',
    },
  },
  {
    id: 'vt',
    name: 'VideoToolbox Encoder',
    options: {
      vt_coder: 'auto',
      vt_software: 'auto',
      vt_realtime: 'enabled',
    },
  },
  {
    id: 'vaapi',
    name: 'VA-API Encoder',
    options: {
      vaapi_strict_rc_buffer: 'disabled',
    },
  },
  {
    id: 'sw',
    name: 'Software Encoder',
    options: {
      sw_preset: 'superfast',
      sw_tune: 'zerolatency',
    },
  },
] as const satisfies ReadonlyArray<{
  id: string;
  name: string;
  options: Record<string, unknown>;
}>;

// Flatten for easy lookup
type DefaultGroups = typeof defaultGroups;
type ConfigDefaults = WidenLiteral<UnionToIntersection<DefaultGroups[number]['options']>>;
type ConfigKey = keyof ConfigDefaults;
type ConfigData = Record<string, unknown>;
export type ConfigState = ConfigDefaults & { platform: string } & Record<string, any>;

function createDefaultMap<T extends readonly { options: Record<string, unknown> }[]>(groups: T) {
  type Result = WidenLiteral<UnionToIntersection<T[number]['options']>>;
  const map = {} as Result;
  for (const g of groups) {
    Object.assign(map as Record<string, unknown>, g.options);
  }
  return map;
}

const defaultMap: ConfigDefaults = createDefaultMap(defaultGroups);

function hasDefaultKey(key: string): key is ConfigKey {
  return Object.prototype.hasOwnProperty.call(defaultMap, key);
}

function deepClone<T>(v: T): T {
  return v === undefined ? v : (JSON.parse(JSON.stringify(v)) as T);
}

function deepEqual<T>(a: T, b: T): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

export const useConfigStore = defineStore('config', () => {
  const tabs = ref(defaultGroups); // keep existing export shape
  const _data = ref<ConfigData | null>(null); // only user/server values
  // Single meta object kept completely separate from user config
  const metadata = ref<MetaInfo>({});
  const config = ref<ConfigState>(buildWrapper()); // wrapper with getters/setters for UI binding
  const version = ref(0); // increments only on real user changes
  // Track keys that should require manual save (no autosave)
  const manualSaveKeys = new Set<string>([
    'global_prep_cmd',
    'dd_resolution_option',
    'dd_manual_resolution',
    'dd_mode_remapping',
  ]);
  const manualDirty = ref(false);
  const savingState = ref<'idle' | 'dirty' | 'saving' | 'saved' | 'error'>('idle');
  const loading = ref(false);
  const error = ref<string | null>(null);
  const validationError = ref<string | null>(null);

  // --- Autosave (PATCH) queue ------------------------------------------------
  // Holds only non-manual changes since last flush. Keys are replaced with
  // the most recent value. Values equal to defaults are converted to null
  // so the server removes them to fall back to default behavior.
  const patchQueue = ref<Record<string, unknown>>({});
  let flushTimer: any = null; // one-shot timer
  let flushInFlight = false;
  const autosaveIntervalMs = 3000;
  const nextFlushAt = ref<number | null>(null); // when the current timer will fire
  const lastSaveResult = ref<{
    appliedNow?: boolean;
    deferred?: boolean;
    restartRequired?: boolean;
  } | null>(null);

  function buildWrapper(): ConfigState {
    const target = {} as ConfigState;
    // union of keys (defaults + current data)
    const keys = new Set<string>([
      ...Object.keys(defaultMap),
      ...Object.keys(_data.value || {}),
      // keep any server-only metadata keys already present
    ]);
    if (_data.value) {
      for (const k of Object.keys(_data.value)) keys.add(k);
    }
    keys.forEach((k) => {
      Object.defineProperty(target, k, {
        enumerable: true,
        configurable: true,
        get() {
          const current = _data.value;
          if (current && Object.prototype.hasOwnProperty.call(current, k)) {
            return current[k];
          }
          // For objects/arrays return a fresh clone so accidental mutation
          // does not silently diverge from persistence. To support in-place
          // mutation (e.g. push) we lazily materialize object/array defaults
          // into _data WITHOUT bumping version (not a user change yet).
          if (hasDefaultKey(k)) {
            const dv = defaultMap[k];
            if (dv && typeof dv === 'object') {
              if (!_data.value) _data.value = {} as ConfigData;
              const storeData = _data.value;
              if (storeData && !Object.prototype.hasOwnProperty.call(storeData, k)) {
                (storeData as Record<string, unknown>)[k] = deepClone(dv);
              }
              return storeData ? storeData[k] : dv;
            }
            return dv;
          }
          return undefined;
        },
        set(v) {
          if (!_data.value) _data.value = {} as ConfigData;
          const prev = _data.value[k];
          if (deepEqual(prev, v)) return; // ignore no-op
          _data.value[k] = v;
          // If this key requires manual save, do not bump version so
          // autosave logic won't trigger; mark manual dirty instead
          if (manualSaveKeys.has(k)) {
            manualDirty.value = true;
            savingState.value = 'dirty';
          } else {
            version.value++;
            savingState.value = 'dirty';
            // queue for patch: send null when value matches default
            let toSend: unknown = v;
            if (hasDefaultKey(k) && deepEqual(v, defaultMap[k])) {
              toSend = null;
            }
            patchQueue.value = { ...patchQueue.value, [k]: toSend };
            // reset autosave timer to full interval on any pending change
            scheduleAutosave();
          }
        },
      });
    });
    // Virtual, read-only platform property sourced from metadata
    Object.defineProperty(target, 'platform', {
      enumerable: true,
      configurable: true,
      get() {
        return metadata.value?.platform || '';
      },
      set(_v) {
        // ignore writes; platform is server-provided only
      },
    });
    return target;
  }

  function setConfig(obj: unknown) {
    // config payload should not include metadata anymore; just clone
    _data.value = (obj ? JSON.parse(JSON.stringify(obj)) : {}) as ConfigData;
    const data = _data.value;

    // decode known JSON string fields
    const specialOptions: Array<keyof ConfigDefaults> = ['dd_mode_remapping', 'global_prep_cmd'];
    for (const key of specialOptions) {
      if (
        data &&
        Object.prototype.hasOwnProperty.call(data, key) &&
        typeof data[key] === 'string'
      ) {
        try {
          data[key] = JSON.parse(data[key] as string);
        } catch {
          /* ignore */
        }
      }
    }

    // Coerce primitive types based on defaults so UI widgets match options.
    // This fixes cases where server returns numeric fields as strings, causing
    // selects to show raw values instead of their friendly labels.
    if (data) {
      for (const key of Object.keys(data)) {
        if (!hasDefaultKey(key)) continue;
        const dv = defaultMap[key];
        const cur = data[key];
        // If default is a number, coerce string numerics to numbers
        if (typeof dv === 'number' && typeof cur === 'string') {
          const n = Number(cur);
          if (Number.isFinite(n)) {
            data[key] = n;
          }
        }
      }
    }

    // Keep frame limiter legacy and new flags in sync so toggles work across versions.
    if (data) {
      if (!Object.prototype.hasOwnProperty.call(data, 'frame_limiter_enable')) {
        (data as Record<string, unknown>)['frame_limiter_enable'] = false;
      }
      if (!Object.prototype.hasOwnProperty.call(data, 'frame_limiter_provider')) {
        (data as Record<string, unknown>)['frame_limiter_provider'] = 'auto';
      }
      const legacyVsync = Object.prototype.hasOwnProperty.call(data, 'rtss_disable_vsync_ullm');
      const hasNewVsync = Object.prototype.hasOwnProperty.call(data, 'frame_limiter_disable_vsync');
      if (legacyVsync) {
        if (!hasNewVsync) {
          (data as Record<string, unknown>)['frame_limiter_disable_vsync'] = (data as Record<
            string,
            unknown
          >)['rtss_disable_vsync_ullm'];
        }
        delete (data as Record<string, unknown>)['rtss_disable_vsync_ullm'];
      }
    }

    // Normalize Playnite boolean-like fields to real booleans so toggles
    // persist as true/false instead of enabled/disabled strings.
    const playniteBoolKeys = [
      'playnite_auto_sync',
      'playnite_sync_all_installed',
      'playnite_autosync_require_replacement',
      'playnite_autosync_remove_uninstalled',
      'playnite_focus_exit_on_first',
      'playnite_fullscreen_entry_enabled',
    ];
    // Extend boolean normalization to cover RTSS enable flag
    const otherBoolKeys = [
      'frame_limiter_enable',
      'frame_limiter_disable_vsync',
      'dd_wa_virtual_double_refresh',
      'dd_wa_dummy_plug_hdr10',
    ];
    const allBoolKeys = playniteBoolKeys.concat(otherBoolKeys);
    const toBool = (v: any): boolean | null => {
      if (v === true || v === false) return v;
      if (v === 1 || v === 0) return !!v;
      const s = String(v ?? '')
        .toLowerCase()
        .trim();
      if (!s) return null;
      if (['true', 'yes', 'enable', 'enabled', 'on', '1'].includes(s)) return true;
      if (['false', 'no', 'disable', 'disabled', 'off', '0'].includes(s)) return false;
      return null;
    };
    if (data) {
      for (const k of allBoolKeys) {
        if (!Object.prototype.hasOwnProperty.call(data, k)) continue;
        const b = toBool(data[k]);
        if (b !== null) {
          data[k] = b;
        }
      }
    }

    if (data && Boolean((data as Record<string, unknown>)['dd_wa_dummy_plug_hdr10'])) {
      (data as Record<string, unknown>)['frame_limiter_disable_vsync'] = true;
    }

    // Normalize Playnite category/exclusion lists to arrays of {id,name}
    const normalizeIdNameArray = (
      v: any,
      treatStringsAsIds: boolean,
    ): Array<{ id: string; name: string }> => {
      const out: Array<{ id: string; name: string }> = [];
      if (Array.isArray(v)) {
        for (const el of v) {
          if (el && typeof el === 'object') {
            const id = String((el as any).id || '');
            const name = String((el as any).name || '');
            if (id || name) out.push({ id, name });
          } else if (typeof el === 'string') {
            const s = el.trim();
            if (!s) continue;
            out.push(treatStringsAsIds ? { id: s, name: '' } : { id: '', name: s });
          }
        }
        return out;
      }
      if (typeof v === 'string') {
        // Try JSON first
        try {
          const parsed = JSON.parse(v);
          return normalizeIdNameArray(parsed, treatStringsAsIds);
        } catch {}
        // CSV fallback
        for (const s of v
          .split(',')
          .map((s) => s.trim())
          .filter(Boolean)) {
          out.push(treatStringsAsIds ? { id: s, name: '' } : { id: '', name: s });
        }
      }
      return out;
    };
    const normalizeStringArray = (v: any): string[] => {
      if (Array.isArray(v)) {
        return v
          .map((item) => String(item ?? '').trim())
          .filter((item) => item.length > 0);
      }
      if (typeof v === 'string') {
        // Try JSON first
        try {
          const parsed = JSON.parse(v);
          return normalizeStringArray(parsed);
        } catch {
          /* ignore */
        }
        return v
          .split(',')
          .map((s) => s.trim())
          .filter((s) => s.length > 0);
      }
      return [];
    };
    if (data) {
      const record = data as Record<string, unknown>;
      if (Object.prototype.hasOwnProperty.call(record, 'playnite_sync_categories')) {
        record['playnite_sync_categories'] = normalizeIdNameArray(
          record['playnite_sync_categories'],
          false,
        );
      }
      if (Object.prototype.hasOwnProperty.call(record, 'playnite_sync_plugins')) {
        record['playnite_sync_plugins'] = normalizeIdNameArray(
          record['playnite_sync_plugins'],
          true,
        );
      }
      if (Object.prototype.hasOwnProperty.call(record, 'playnite_exclude_categories')) {
        record['playnite_exclude_categories'] = normalizeIdNameArray(
          record['playnite_exclude_categories'],
          false,
        );
      }
      if (Object.prototype.hasOwnProperty.call(record, 'playnite_exclude_games')) {
        record['playnite_exclude_games'] = normalizeIdNameArray(
          record['playnite_exclude_games'],
          true,
        );
      }
      if (Object.prototype.hasOwnProperty.call(record, 'dd_snapshot_exclude_devices')) {
        record['dd_snapshot_exclude_devices'] = normalizeStringArray(
          record['dd_snapshot_exclude_devices'],
        );
      }
    }

    config.value = buildWrapper();
  }

  function updateOption<K extends ConfigKey>(key: K, value: ConfigDefaults[K]): void;
  function updateOption(key: string, value: unknown): void;
  function updateOption(key: string, value: unknown) {
    (config.value as Record<string, unknown>)[key] = value; // triggers setter (handles manual/auto)
  }

  // Explicitly mark a manual-dirty change (e.g., when mutating nested fields)
  function markManualDirty(_key?: string) {
    manualDirty.value = true;
    savingState.value = 'dirty';
  }

  function resetManualDirty() {
    manualDirty.value = false;
  }

  function validateManualSave(): { ok: true } | { ok: false; message: string } {
    if (!manualDirty.value) return { ok: true };
    const data = (_data.value ?? {}) as Record<string, unknown>;

    const resolutionOptionKey = 'dd_resolution_option' as const;
    const defaultResolutionOption = hasDefaultKey(resolutionOptionKey)
      ? defaultMap[resolutionOptionKey]
      : undefined;
    const resOpt = Object.prototype.hasOwnProperty.call(data, resolutionOptionKey)
      ? data[resolutionOptionKey]
      : defaultResolutionOption;
    if (resOpt === 'manual') {
      const manualResolutionKey = 'dd_manual_resolution' as const;
      const raw = String(data[manualResolutionKey] ?? '').trim();
      const resolutionPattern = /^\d{2,5}\s*[xX]\s*\d{2,5}$/;
      if (!resolutionPattern.test(raw)) {
        return {
          ok: false,
          message: 'Invalid manual resolution. Use WIDTHxHEIGHT (e.g., 2560x1440).',
        };
      }
    }

    const refreshOptionKey = 'dd_refresh_rate_option' as const;
    const defaultRefreshOption = hasDefaultKey(refreshOptionKey)
      ? defaultMap[refreshOptionKey]
      : undefined;
    const rrOpt = Object.prototype.hasOwnProperty.call(data, refreshOptionKey)
      ? data[refreshOptionKey]
      : defaultRefreshOption;
    if (rrOpt === 'manual') {
      const manualRefreshKey = 'dd_manual_refresh_rate' as const;
      const raw = String(data[manualRefreshKey] ?? '').trim();
      const valid = /^\d+(?:\.\d+)?$/.test(raw) && Number(raw) > 0;
      if (!valid) {
        return {
          ok: false,
          message: 'Invalid manual refresh rate. Use a positive number, e.g., 60 or 59.94.',
        };
      }
    }

    const remap = data['dd_mode_remapping'];
    if (remap && typeof remap === 'object') {
      const remapObj = remap as Record<string, unknown>;
      const resolutionPattern = /^\d{2,5}\s*[xX]\s*\d{2,5}$/;
      const checkResolution = (value: unknown) =>
        !value || String(value).trim() === '' || resolutionPattern.test(String(value));
      const checkNumber = (value: unknown) =>
        !value ||
        String(value).trim() === '' ||
        (/^\d+(?:\.\d+)?$/.test(String(value)) && Number(value) > 0);

      const resolutionBuckets = ['mixed', 'resolution_only'] as const;
      for (const bucket of resolutionBuckets) {
        const entries = Array.isArray(remapObj[bucket]) ? (remapObj[bucket] as unknown[]) : [];
        for (const entry of entries) {
          const item = entry as Record<string, unknown>;
          if (
            !checkResolution(item?.['requested_resolution']) ||
            !checkResolution(item?.['final_resolution'])
          ) {
            return {
              ok: false,
              message:
                'Invalid resolution in Display mode remapping. Use WIDTHxHEIGHT (e.g., 1920x1080) or leave blank.',
            };
          }
        }
      }

      const refreshOnly = Array.isArray(remapObj['refresh_rate_only'])
        ? (remapObj['refresh_rate_only'] as unknown[])
        : [];
      for (const entry of refreshOnly) {
        const item = entry as Record<string, unknown>;
        if (!checkNumber(item?.['requested_fps']) || !checkNumber(item?.['final_refresh_rate'])) {
          return {
            ok: false,
            message: 'Invalid refresh rate in remapping. Use a positive number or leave blank.',
          };
        }
        const finalRate = item?.['final_refresh_rate'];
        if (!finalRate || String(finalRate).trim() === '') {
          return {
            ok: false,
            message: 'For refresh-rate-only mappings, Final refresh rate is required.',
          };
        }
      }

      const mixed = Array.isArray(remapObj['mixed']) ? (remapObj['mixed'] as unknown[]) : [];
      for (const entry of mixed) {
        const item = entry as Record<string, unknown>;
        if (!checkNumber(item?.['requested_fps']) || !checkNumber(item?.['final_refresh_rate'])) {
          return {
            ok: false,
            message: 'Invalid refresh rate in remapping. Use a positive number or leave blank.',
          };
        }
        const finalRes = item?.['final_resolution'];
        const finalFps = item?.['final_refresh_rate'];
        const hasFinalRes = !!finalRes && String(finalRes).trim() !== '';
        const hasFinalFps = !!finalFps && String(finalFps).trim() !== '';
        if (!hasFinalRes && !hasFinalFps) {
          return {
            ok: false,
            message: 'For mixed mappings, specify at least one Final field.',
          };
        }
      }

      const resolutionOnly = Array.isArray(remapObj['resolution_only'])
        ? (remapObj['resolution_only'] as unknown[])
        : [];
      for (const entry of resolutionOnly) {
        const item = entry as Record<string, unknown>;
        const finalRes = item?.['final_resolution'];
        if (!finalRes || String(finalRes).trim() === '') {
          return {
            ok: false,
            message: 'For resolution-only mappings, Final resolution is required.',
          };
        }
      }
    }

    return { ok: true };
  }

  async function save(): Promise<boolean> {
    try {
      // Validate manual-save fields before attempting to persist
      const v = validateManualSave();
      if (!v.ok) {
        validationError.value = v.message || 'Validation failed for pending changes.';
        savingState.value = 'error';
        return false;
      }
      // First flush any pending PATCH changes for auto-saved keys
      if (Object.keys(patchQueue.value).length) {
        const ok = await flushPatchQueue();
        if (!ok) return false;
      }
      savingState.value = 'saving';
      const body = serialize();
      const res = await http.post('/api/config', body || {}, {
        headers: { 'Content-Type': 'application/json' },
        validateStatus: () => true,
      });
      if (res.status === 200) {
        try {
          lastSaveResult.value = {
            appliedNow: !!(res as any)?.data?.appliedNow,
            deferred: !!(res as any)?.data?.deferred,
            restartRequired: !!(res as any)?.data?.restartRequired,
          };
        } catch {}
        savingState.value = 'saved';
        manualDirty.value = false;
        validationError.value = null;
        // Reset to idle after a short delay if no new changes
        setTimeout(() => {
          if (savingState.value === 'saved' && !manualDirty.value) {
            savingState.value = 'idle';
          }
        }, 3000);
        return true;
      }
      savingState.value = 'error';
      return false;
    } catch (e) {
      savingState.value = 'error';
      return false;
    }
  }

  function serialize(): Record<string, unknown> | null {
    if (!_data.value) return null;
    const out: Record<string, unknown> = JSON.parse(JSON.stringify(_data.value));
    // prune defaults (value exactly equals default)
    for (const k of Object.keys(out)) {
      if (hasDefaultKey(k) && deepEqual(out[k], defaultMap[k])) delete out[k];
    }
    // never persist virtual keys
    delete out['platform'];
    return out;
  }

  async function fetchConfig(force = false) {
    if (_data.value && !force) return config.value;
    loading.value = true;
    error.value = null;
    try {
      const r = await http.get('/api/config');
      if (r.status !== 200) throw new Error('bad status ' + r.status);
      // Fetch metadata (non-fatal if it fails)
      try {
        const mr = await http.get('/api/metadata');
        if (mr.status === 200 && mr.data) {
          const m = { ...mr.data } as MetaInfo;
          // Normalize platform identifiers across build/runtime variations
          const raw = String((m as any).platform || '').toLowerCase();
          let norm = raw;
          if (raw.startsWith('win')) norm = 'windows';
          else if (raw === 'darwin' || raw.startsWith('mac')) norm = 'macos';
          else if (raw.startsWith('lin')) norm = 'linux';
          (m as any).platform = norm;
          metadata.value = m;
        }
      } catch (_) {
        /* ignore */
      }
      // keep settings and metadata separate
      setConfig(r.data);
      return config.value;
    } catch (e: any) {
      console.error('fetchConfig failed', e);
      error.value = e?.message || 'fetch failed';
      return null;
    } finally {
      loading.value = false;
    }
  }

  async function flushPatchQueue(): Promise<boolean> {
    if (flushInFlight) return true;
    const payload = patchQueue.value;
    if (!payload || Object.keys(payload).length === 0) return true;
    // Clear queue immediately (we'll merge any new changes on top if request fails)
    patchQueue.value = {};
    flushInFlight = true;
    // Clear any scheduled timer since we're flushing now
    if (flushTimer) clearTimeout(flushTimer);
    flushTimer = null;
    nextFlushAt.value = null;
    try {
      savingState.value = 'saving';
      const res = await http.patch('/api/config', payload, {
        headers: { 'Content-Type': 'application/json' },
        validateStatus: () => true,
      });
      if (res.status === 200) {
        try {
          lastSaveResult.value = {
            appliedNow: !!(res as any)?.data?.appliedNow,
            deferred: !!(res as any)?.data?.deferred,
            restartRequired: !!(res as any)?.data?.restartRequired,
          };
        } catch {}
        savingState.value = 'saved';
        setTimeout(() => {
          if (
            savingState.value === 'saved' &&
            !manualDirty.value &&
            Object.keys(patchQueue.value).length === 0
          ) {
            savingState.value = 'idle';
          }
        }, 3000);
        return true;
      }
      savingState.value = 'error';
      return false;
    } catch (e) {
      savingState.value = 'error';
      return false;
    } finally {
      flushInFlight = false;
    }
  }

  function startAutosave() {
    // no-op; autosave uses a debounced one-shot timer via scheduleAutosave()
  }

  function stopAutosave() {
    if (flushTimer) clearTimeout(flushTimer);
    flushTimer = null;
    nextFlushAt.value = null;
  }

  async function reloadConfig() {
    _data.value = null;
    return await fetchConfig(true);
  }

  // Start autosave queue watcher by default
  startAutosave();

  function hasPendingPatch() {
    return Object.keys(patchQueue.value).length > 0;
  }
  function nextAutosaveAt(): number {
    return nextFlushAt.value || 0;
  }

  function scheduleAutosave() {
    if (flushTimer) clearTimeout(flushTimer);
    nextFlushAt.value = Date.now() + autosaveIntervalMs;
    flushTimer = setTimeout(() => {
      nextFlushAt.value = null;
      if (Object.keys(patchQueue.value).length === 0) return;
      void flushPatchQueue();
    }, autosaveIntervalMs);
  }

  return {
    // state
    tabs,
    defaults: defaultMap,
    config: config as unknown as ConfigState, // exposed as value for direct usage
    version, // increments only on user mutation
    manualDirty,
    savingState,
    metadata,
    loading,
    error,
    validationError,
    fetchConfig,
    setConfig,
    updateOption,
    markManualDirty,
    resetManualDirty,
    save,
    serialize,
    // queue/autosave utils
    flushPatchQueue,
    startAutosave,
    stopAutosave,
    reloadConfig,
    hasPendingPatch,
    autosaveIntervalMs,
    nextAutosaveAt,
    lastSaveResult,
  };
});
