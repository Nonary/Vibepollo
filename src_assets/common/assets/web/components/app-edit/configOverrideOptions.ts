import {
  buildConfigOptionsText,
  getConfigSelectOptions,
  type ConfigSelectOption,
  type ConfigSelectOptionsContext,
} from '@/configs/configSelectOptions';

export type OverrideSelectOption = ConfigSelectOption;
export type OverrideSelectOptionsContext = ConfigSelectOptionsContext;

function isOverrideSelectValue(value: unknown): value is string | number {
  return typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value));
}

const BOOLEAN_OVERRIDE_KEYS = new Set<string>([
  'always_send_scancodes',
  'amd_enforce_hrd',
  'amd_preanalysis',
  'controller',
  'dd_activate_virtual_display',
  'dd_always_restore_from_golden',
  'dd_config_revert_on_disconnect',
  'dd_wa_dummy_plug_hdr10',
  'ds4_back_as_touchpad_click',
  'frame_limiter_disable_vsync',
  'frame_limiter_enable',
  'high_resolution_scrolling',
  'key_rightalt_to_key_win',
  'keyboard',
  'motion_as_ds4',
  'mouse',
  'native_pen_touch',
  'nvenc_h264_cavlc',
  'nvenc_latency_over_power',
  'nvenc_opengl_vulkan_on_dxgi',
  'nvenc_realtime_hags',
  'nvenc_spatial_aq',
  'qsv_slow_hevc',
  'rtx_hdr',
  'rtx_hdr_force_sdr',
  'stream_audio',
  'touchpad_as_ds4',
  'vaapi_strict_rc_buffer',
  'vt_realtime',
]);

export function isBooleanOverrideKey(key: string): boolean {
  return BOOLEAN_OVERRIDE_KEYS.has(key);
}

export function getOverrideSelectOptions(
  key: string,
  ctx: OverrideSelectOptionsContext,
): OverrideSelectOption[] {
  const seen = new Set<string>();
  const options: OverrideSelectOption[] = [];

  for (const option of getConfigSelectOptions(key, ctx)) {
    const value = String(option.value);
    if (seen.has(value)) continue;
    seen.add(value);
    options.push({ ...option, value });
  }

  // Keep an unrecognized saved value visible inside a real dropdown, but only
  // when the key actually declares choices. Synthesizing an option for a key
  // with no declared list turns every numeric and free-text override into a
  // dead single-entry select that cannot be typed into.
  // Known boolean settings use the checkbox renderer even when their serialized
  // value is a string or 0/1. Declared select choices above still take priority.
  if (options.length > 0 && isOverrideSelectValue(ctx.currentValue) && !isBooleanOverrideKey(key)) {
    const value = String(ctx.currentValue);
    if (!seen.has(value)) {
      options.push({ label: value, value });
    }
  }

  return options;
}

export function buildOverrideOptionsText(options: OverrideSelectOption[]): string {
  return buildConfigOptionsText(options);
}
