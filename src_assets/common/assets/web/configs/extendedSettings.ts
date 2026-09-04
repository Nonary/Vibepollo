import { getConfigFieldDefinition } from './configFieldSchema.ts';
import type { SettingsField, SettingsGroup } from './settingsSchema.ts';

// Defaults and wire values match the existing host configuration and legacy editors.
export const extendedDefaults: Record<string, unknown> = {
  amd_ltr_frames: 0,
  amd_input_queue_size: 0,
  amd_smart_access_video: 'auto',
  amd_lowlatency_mode: 'auto',
  amd_high_motion_quality_boost: 'auto',
  playnite_install_dir: '',
  playnite_extensions_dir: '',

  nvenc_preset: 1,
  nvenc_twopass: 'quarter_res',
  nvenc_spatial_aq: 'disabled',
  nvenc_split_encode: 'auto',
  nvenc_vbv_increase: 0,
  nvenc_realtime_hags: 'enabled',
  nvenc_latency_over_power: 'enabled',
  nvenc_opengl_vulkan_on_dxgi: 'enabled',
  nvenc_h264_cavlc: 'disabled',
  qsv_preset: 'medium',
  qsv_coder: 'auto',
  qsv_slow_hevc: 'disabled',
  amd_usage: 'ultralowlatency',
  amd_rc: 'vbr_latency',
  amd_qvbr_quality_level: 0,
  amd_enforce_hrd: 'disabled',
  amd_quality: 'balanced',
  amd_preanalysis: 'disabled',
  amd_vbaq: 'enabled',
  amd_coder: 'auto',
  amd_av1_screen_content: 'auto',
  amd_av1_latency_mode: 'auto',
  vt_coder: 'auto',
  vt_software: 'auto',
  vt_realtime: 'enabled',
  vaapi_strict_rc_buffer: 'disabled',
  vk_tune: 2,
  vk_rc_mode: 2,
  sw_preset: 'superfast',
  sw_tune: 'zerolatency',
  min_threads: 2,
  locale: 'en',
  update_check_interval: 86400,
  session_token_ttl_seconds: 86400,
  remember_me_refresh_token_ttl_seconds: 604800,
  back_button_timeout: -1,
  key_rightalt_to_key_win: 'disabled',
  ds5_inputtino_randomize_mac: true,
  credentials_file: '',
  file_state: '',
  realtime_stats_enabled: true,
  realtime_stats_poll_interval_ms: 2000,
  realtime_stats_history_retention_seconds: 300,
  realtime_stats_max_history_points: 300,
  realtime_stats_pause_when_hidden: true,
  realtime_stats_show_active_sessions: true,
  realtime_stats_show_host_stats: true,
  realtime_stats_show_host_charts: true,
  realtime_stats_show_session_history: true,
  rtx_hdr: false,
  rtx_hdr_force_sdr: false,
  rtx_hdr_peak_brightness: 1000,
  rtx_hdr_sdr_brightness: 0,
  rtx_hdr_middle_gray: 50,
  rtx_hdr_contrast: 0,
  rtx_hdr_saturation: 0,
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
};

export function extendedField(key: string, extra: Partial<SettingsField> = {}): SettingsField {
  const definition = getConfigFieldDefinition(key, {
    t: (key) => key,
    platform: '',
    defaultValue: extendedDefaults[key],
  });
  const kind =
    definition.kind === 'checkbox' || definition.kind === 'switch'
      ? 'boolean'
      : definition.kind === 'number' || definition.kind === 'slider'
        ? 'number'
        : definition.kind === 'select'
          ? 'select'
          : 'text';
  return {
    key,
    kind: [
      'amd_smart_access_video',
      'amd_lowlatency_mode',
      'amd_high_motion_quality_boost',
    ].includes(key)
      ? 'select'
      : kind,
    min: definition.min,
    max: definition.max,
    step: definition.step,
    options: definition.options?.map((option) => ({
      value: String(option.value),
      labelKey: option.label,
    })),
    ...extra,
    ...(key === 'amd_ltr_frames'
      ? { min: 0, max: 2, step: 1 }
      : key === 'amd_input_queue_size'
        ? { min: 0, max: 32, step: 1 }
        : {}),
    ...(['amd_smart_access_video', 'amd_lowlatency_mode', 'amd_high_motion_quality_boost'].includes(
      key,
    )
      ? {
          options: ['auto', 'enabled', 'disabled'].map((value) => ({
            value,
            labelKey: `_common.${value}`,
          })),
        }
      : {}),
  };
}

const encoderFamilies = {
  nvenc: 'nvidia',
  qsv: 'intel',
  amd: 'amd',
  vt: 'videotoolbox',
  vaapi: 'vaapi',
  vk: 'vulkan',
  sw: 'software',
} as const;
export const advancedEncoderGroups: SettingsGroup[] = Object.entries(encoderFamilies).map(
  ([prefix, encoderFamily]) => ({
    id: `encoder_${prefix}`,
    collapsed: true,
    fields: Object.keys(extendedDefaults)
      .filter((key) => key.startsWith(`${prefix}_`))
      .map((key) =>
        extendedField(key, {
          encoderFamily,
          platform:
            ['qsv', 'amd'].includes(prefix) ||
            ['nvenc_realtime_hags', 'nvenc_opengl_vulkan_on_dxgi'].includes(key)
              ? 'windows'
              : prefix === 'vt'
                ? 'macos'
                : ['vaapi', 'vk'].includes(prefix)
                  ? 'linux'
                  : undefined,
        }),
      ),
  }),
);

export const playnitePolicyFields = Object.keys(extendedDefaults)
  .filter((key) => key.startsWith('playnite_'))
  .map((key) =>
    extendedField(key, {
      platform: 'windows',
      ...(typeof extendedDefaults[key] === 'number' ? { min: 0, step: 1 } : {}),
    }),
  );
