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
}

// --- Defaults (flat) -------------------------------------------------------
// Keep these separate from runtime state so reading defaults does NOT mutate
// the actual config object that will be POSTed back to the server.
const defaultGroups = [
  {
    id: 'general',
    name: 'General',
    options: {
      locale: 'en',
      sunshine_name: '',
      min_log_level: 2,
      global_prep_cmd: [],
      notify_pre_releases: 'disabled',
      update_check_interval: 86400,
      session_token_ttl_seconds: 86400,
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
    },
  },
  {
    id: 'av',
    name: 'Audio/Video',
    options: {
      audio_sink: '',
      virtual_sink: '',
      install_steam_audio_drivers: 'enabled',
      adapter_name: '',
      output_name: '',
      dd_configuration_option: 'disabled',
      dd_resolution_option: 'auto',
      dd_manual_resolution: '',
      dd_refresh_rate_option: 'auto',
      dd_manual_refresh_rate: '',
      dd_hdr_option: 'auto',
      dd_config_revert_delay: 3000,
      dd_config_revert_on_disconnect: 'disabled',
      dd_mode_remapping: { mixed: [], resolution_only: [], refresh_rate_only: [] },
      dd_wa_hdr_toggle_delay: 0,
      max_bitrate: 0,
      minimum_fps_target: 0,
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
    },
  },
  {
    id: 'playnite',
    name: 'Playnite',
    options: {
      playnite_auto_sync: true,
      playnite_recent_games: 10,
      playnite_recent_max_age_days: 0,
      playnite_autosync_delete_after_days: 0,
      playnite_autosync_require_replacement: true,
      playnite_focus_attempts: 3,
      playnite_focus_timeout_secs: 15,
      playnite_focus_exit_on_first: false,
      playnite_sync_categories: [],
      playnite_exclude_games: [],
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
      capture: '',
      encoder: '',
    },
  },
  {
    id: 'nv',
    name: 'NVIDIA NVENC Encoder',
    options: {
      nvenc_preset: 1,
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
];

// Flatten for easy lookup
const defaultMap: Record<string, any> = {};
for (const g of defaultGroups) Object.assign(defaultMap, g.options);

function deepClone(v: any) {
  return v === undefined ? v : JSON.parse(JSON.stringify(v));
}

function deepEqual(a: any, b: any): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

export const useConfigStore = defineStore('config', () => {
  const tabs = ref(defaultGroups); // keep existing export shape
  const _data = ref<Record<string, any> | null>(null); // only user/server values
  const config = ref<any>(null); // wrapper with getters/setters for UI binding
  const version = ref(0); // increments only on real user changes
  // Track keys that should require manual save (no autosave)
  const manualSaveKeys = new Set<string>(['global_prep_cmd']);
  const manualDirty = ref(false);
  const savingState = ref<'idle' | 'dirty' | 'saving' | 'saved' | 'error'>('idle');
  const loading = ref(false);
  const error = ref<string | null>(null);
  // Single meta object kept completely separate from user config
  const metadata = ref<MetaInfo>({});

  // --- Autosave (PATCH) queue ------------------------------------------------
  // Holds only non-manual changes since last flush. Keys are replaced with
  // the most recent value. Values equal to defaults are converted to null
  // so the server removes them to fall back to default behavior.
  const patchQueue = ref<Record<string, any>>({});
  let flushTimer: any = null; // one-shot timer
  let flushInFlight = false;
  const autosaveIntervalMs = 3000;
  const nextFlushAt = ref<number | null>(null); // when the current timer will fire
  const lastSaveResult = ref<{
    appliedNow?: boolean;
    deferred?: boolean;
    restartRequired?: boolean;
  } | null>(null);

  function buildWrapper() {
    const target: any = {};
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
          if (_data.value && Object.prototype.hasOwnProperty.call(_data.value, k)) {
            return _data.value[k];
          }
          // For objects/arrays return a fresh clone so accidental mutation
          // does not silently diverge from persistence. To support in-place
          // mutation (e.g. push) we lazily materialize object/array defaults
          // into _data WITHOUT bumping version (not a user change yet).
          const dv = defaultMap[k];
          if (dv && typeof dv === 'object') {
            if (!_data.value) _data.value = {};
            _data.value[k] = deepClone(dv);
            return _data.value[k];
          }
          return dv;
        },
        set(v) {
          if (!_data.value) _data.value = {};
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
            const dv = (defaultMap as any)[k];
            const toSend = deepEqual(v, dv) ? null : v;
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

  function setConfig(obj: any) {
    // config payload should not include metadata anymore; just clone
    _data.value = obj ? JSON.parse(JSON.stringify(obj)) : {};

    // decode known JSON string fields
    const specialOptions = ['dd_mode_remapping', 'global_prep_cmd'];
    for (const key of specialOptions) {
      if (
        _data.value &&
        Object.prototype.hasOwnProperty.call(_data.value, key) &&
        typeof _data.value[key] === 'string'
      ) {
        try {
          _data.value[key] = JSON.parse(_data.value[key]);
        } catch {
          /* ignore */
        }
      }
    }

    // Coerce primitive types based on defaults so UI widgets match options.
    // This fixes cases where server returns numeric fields as strings, causing
    // selects to show raw values instead of their friendly labels.
    if (_data.value) {
      for (const key of Object.keys(_data.value)) {
        const dv = (defaultMap as any)[key];
        const cur = (_data.value as any)[key];
        // If default is a number, coerce string numerics to numbers
        if (typeof dv === 'number' && typeof cur === 'string') {
          const n = Number(cur);
          if (Number.isFinite(n)) {
            (_data.value as any)[key] = n;
          }
        }
      }
    }

    // Normalize Playnite boolean-like fields to real booleans so toggles
    // persist as true/false instead of enabled/disabled strings.
    const playniteBoolKeys = [
      'playnite_auto_sync',
      'playnite_autosync_require_replacement',
      'playnite_focus_exit_on_first',
    ];
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
    for (const k of playniteBoolKeys) {
      if (!_data.value) break;
      if (Object.prototype.hasOwnProperty.call(_data.value, k)) {
        const b = toBool((_data.value as any)[k]);
        if (b !== null) {
          (_data.value as any)[k] = b;
        }
      }
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
    if (_data.value) {
      if (Object.prototype.hasOwnProperty.call(_data.value, 'playnite_sync_categories')) {
        (_data.value as any).playnite_sync_categories = normalizeIdNameArray(
          (_data.value as any).playnite_sync_categories,
          false,
        );
      }
      if (Object.prototype.hasOwnProperty.call(_data.value, 'playnite_exclude_games')) {
        (_data.value as any).playnite_exclude_games = normalizeIdNameArray(
          (_data.value as any).playnite_exclude_games,
          true,
        );
      }
    }

    config.value = buildWrapper();
  }

  function updateOption(key: string, value: any) {
    if (!config.value) return;
    config.value[key] = value; // triggers setter (handles manual/auto)
  }

  // Explicitly mark a manual-dirty change (e.g., when mutating nested fields)
  function markManualDirty(_key?: string) {
    manualDirty.value = true;
    savingState.value = 'dirty';
  }

  function resetManualDirty() {
    manualDirty.value = false;
  }

  async function save(): Promise<boolean> {
    try {
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

  function serialize(): Record<string, any> | null {
    if (!_data.value) return null;
    const out: Record<string, any> = JSON.parse(JSON.stringify(_data.value));
    // prune defaults (value exactly equals default)
    for (const k of Object.keys(out)) {
      if (k in defaultMap && deepEqual(out[k], defaultMap[k])) delete out[k];
    }
    // never persist virtual keys
    delete (out as any).platform;
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
    config, // ref to wrapper for template access
    version, // increments only on user mutation
    manualDirty,
    savingState,
    metadata,
    loading,
    error,
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
