export type OverrideSelectOption = { label: string; value: string | number };

export type OverrideSelectOptionsContext = {
  t: (key: string) => string;
  platform: string;
  metadata?: any;
  currentValue?: unknown;
};

function tOr(t: (key: string) => string, key: string, fallback: string): string {
  const v = t(key);
  if (!v || v === key) return fallback;
  return v;
}

function isSelectValue(value: unknown): value is string | number {
  return typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value));
}

function ensureIncludesCurrentValue(
  options: OverrideSelectOption[],
  currentValue: unknown,
): OverrideSelectOption[] {
  if (!isSelectValue(currentValue)) return options;
  if (options.some((o) => o.value === currentValue)) return options;
  return options.concat([{ label: String(currentValue), value: currentValue }]);
}

function gpuFlags(metadata: any) {
  const gpus = Array.isArray(metadata?.gpus) ? metadata.gpus : [];
  const hasVendor = (vendorId: number) =>
    gpus.some((gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === vendorId);

  const metaN = metadata?.has_nvidia_gpu;
  const metaI = metadata?.has_intel_gpu;
  const metaA = metadata?.has_amd_gpu;

  const hasNvidia = typeof metaN === 'boolean' ? metaN : gpus.length ? hasVendor(0x10de) : true;
  const hasIntel = typeof metaI === 'boolean' ? metaI : gpus.length ? hasVendor(0x8086) : true;
  const hasAmd =
    typeof metaA === 'boolean'
      ? metaA
      : gpus.length
        ? gpus.some((gpu: any) => {
            const vendor = Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0);
            return vendor === 0x1002 || vendor === 0x1022;
          })
        : true;

  return { hasNvidia, hasIntel, hasAmd };
}

export function getOverrideSelectOptions(
  key: string,
  ctx: OverrideSelectOptionsContext,
): OverrideSelectOption[] {
  const platform = String(ctx.platform || '').toLowerCase();
  const { t } = ctx;

  switch (key) {
    case 'hevc_mode': {
      const options = [0, 1, 2, 3].map((v) => ({
        label: tOr(t, `config.hevc_mode_${v}`, String(v)),
        value: v,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'av1_mode': {
      const options = [0, 1, 2, 3].map((v) => ({
        label: tOr(t, `config.av1_mode_${v}`, String(v)),
        value: v,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'gamepad': {
      const labelMap: Record<string, string> = {
        auto: '_common.auto',
        ds4: 'config.gamepad_ds4',
        ds5: 'config.gamepad_ds5',
        switch: 'config.gamepad_switch',
        x360: 'config.gamepad_x360',
        xone: 'config.gamepad_xone',
      };

      const prioritizedByPlatform: Record<string, string[]> = {
        linux: ['ds5', 'xone', 'switch', 'x360'],
        windows: ['x360', 'ds4'],
      };
      const fallbackOrder = ['x360', 'ds5', 'ds4'];

      const opts: OverrideSelectOption[] = [
        { label: tOr(t, '_common.auto', 'Auto'), value: 'auto' },
      ];
      const seen = new Set<string>(opts.map((o) => String(o.value)));

      const add = (value: string | undefined) => {
        if (!value || seen.has(value)) return;
        const labelKey = labelMap[value] || `config.gamepad_${value}`;
        opts.push({ label: tOr(t, labelKey, value), value });
        seen.add(value);
      };

      const order = prioritizedByPlatform[platform] ?? fallbackOrder;
      order.forEach(add);
      if (typeof ctx.currentValue === 'string' && ctx.currentValue !== 'auto')
        add(ctx.currentValue);
      return opts;
    }
    case 'capture': {
      const opts: OverrideSelectOption[] = [
        { label: tOr(t, '_common.autodetect', 'Autodetect'), value: '' },
      ];
      if (platform === 'windows') {
        opts.push(
          { label: 'Windows Graphics Capture (variable)', value: 'wgc' },
          { label: 'Windows Graphics Capture (constant)', value: 'wgcc' },
          { label: 'Desktop Duplication API', value: 'ddx' },
        );
      } else if (platform === 'linux') {
        opts.push(
          { label: 'NvFBC', value: 'nvfbc' },
          { label: 'wlroots', value: 'wlr' },
          { label: 'KMS', value: 'kms' },
          { label: 'X11', value: 'x11' },
        );
      }
      return ensureIncludesCurrentValue(opts, ctx.currentValue);
    }
    case 'encoder': {
      const opts: OverrideSelectOption[] = [
        { label: tOr(t, '_common.autodetect', 'Autodetect'), value: '' },
      ];
      const { hasNvidia, hasIntel, hasAmd } = gpuFlags(ctx.metadata);
      if (platform === 'windows') {
        if (hasNvidia) opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' });
        if (hasIntel) opts.push({ label: 'Intel QuickSync', value: 'quicksync' });
        if (hasAmd) opts.push({ label: 'AMD AMF/VCE', value: 'amdvce' });
      } else if (platform === 'linux') {
        opts.push({ label: 'NVIDIA NVENC', value: 'nvenc' }, { label: 'VA-API', value: 'vaapi' });
      } else if (platform === 'macos') {
        opts.push({ label: 'VideoToolbox', value: 'videotoolbox' });
      }
      opts.push({ label: tOr(t, 'config.encoder_software', 'Software'), value: 'software' });
      return ensureIncludesCurrentValue(opts, ctx.currentValue);
    }
    case 'nvenc_preset': {
      const extra = (id: 1 | 4 | 7) => {
        const key = `config.nvenc_preset_${id}`;
        const v = t(key);
        return v && v !== key ? v : '';
      };
      const options: OverrideSelectOption[] = [
        { label: `P1 ${extra(1)}`.trim(), value: 1 },
        { label: 'P2', value: 2 },
        { label: 'P3', value: 3 },
        { label: `P4 ${extra(4)}`.trim(), value: 4 },
        { label: 'P5', value: 5 },
        { label: 'P6', value: 6 },
        { label: `P7 ${extra(7)}`.trim(), value: 7 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_twopass': {
      const options = [
        { label: tOr(t, 'config.nvenc_twopass_disabled', 'Disabled'), value: 'disabled' },
        {
          label: tOr(t, 'config.nvenc_twopass_quarter_res', 'Quarter res'),
          value: 'quarter_res',
        },
        { label: tOr(t, 'config.nvenc_twopass_full_res', 'Full res'), value: 'full_res' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_preset': {
      const options = [
        { label: tOr(t, 'config.qsv_preset_veryfast', 'veryfast'), value: 'veryfast' },
        { label: tOr(t, 'config.qsv_preset_faster', 'faster'), value: 'faster' },
        { label: tOr(t, 'config.qsv_preset_fast', 'fast'), value: 'fast' },
        { label: tOr(t, 'config.qsv_preset_medium', 'medium'), value: 'medium' },
        { label: tOr(t, 'config.qsv_preset_slow', 'slow'), value: 'slow' },
        { label: tOr(t, 'config.qsv_preset_slower', 'slower'), value: 'slower' },
        { label: tOr(t, 'config.qsv_preset_slowest', 'slowest'), value: 'slowest' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_coder':
    case 'amd_coder':
    case 'vt_coder': {
      const options = [
        { label: tOr(t, 'config.ffmpeg_auto', 'Auto'), value: 'auto' },
        { label: tOr(t, 'config.coder_cabac', 'CABAC'), value: 'cabac' },
        { label: tOr(t, 'config.coder_cavlc', 'CAVLC'), value: 'cavlc' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_usage': {
      const options = [
        { label: tOr(t, 'config.amd_usage_transcoding', 'Transcoding'), value: 'transcoding' },
        { label: tOr(t, 'config.amd_usage_webcam', 'Webcam'), value: 'webcam' },
        {
          label: tOr(t, 'config.amd_usage_lowlatency_high_quality', 'Low latency (high quality)'),
          value: 'lowlatency_high_quality',
        },
        { label: tOr(t, 'config.amd_usage_lowlatency', 'Low latency'), value: 'lowlatency' },
        {
          label: tOr(t, 'config.amd_usage_ultralowlatency', 'Ultra low latency'),
          value: 'ultralowlatency',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_rc': {
      const options = [
        { label: tOr(t, 'config.amd_rc_cbr', 'CBR'), value: 'cbr' },
        { label: tOr(t, 'config.amd_rc_cqp', 'CQP'), value: 'cqp' },
        { label: tOr(t, 'config.amd_rc_vbr_latency', 'VBR (latency)'), value: 'vbr_latency' },
        { label: tOr(t, 'config.amd_rc_vbr_peak', 'VBR (peak)'), value: 'vbr_peak' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_quality': {
      const options = [
        { label: tOr(t, 'config.amd_quality_speed', 'Speed'), value: 'speed' },
        { label: tOr(t, 'config.amd_quality_balanced', 'Balanced'), value: 'balanced' },
        { label: tOr(t, 'config.amd_quality_quality', 'Quality'), value: 'quality' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'vt_software': {
      const options = [
        { label: tOr(t, '_common.auto', 'Auto'), value: 'auto' },
        { label: tOr(t, '_common.disabled', 'Disabled'), value: 'disabled' },
        { label: tOr(t, 'config.vt_software_allowed', 'Allowed'), value: 'allowed' },
        { label: tOr(t, 'config.vt_software_forced', 'Forced'), value: 'forced' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_preset': {
      const options = [
        { label: tOr(t, 'config.sw_preset_ultrafast', 'ultrafast'), value: 'ultrafast' },
        { label: tOr(t, 'config.sw_preset_superfast', 'superfast'), value: 'superfast' },
        { label: tOr(t, 'config.sw_preset_veryfast', 'veryfast'), value: 'veryfast' },
        { label: tOr(t, 'config.sw_preset_faster', 'faster'), value: 'faster' },
        { label: tOr(t, 'config.sw_preset_fast', 'fast'), value: 'fast' },
        { label: tOr(t, 'config.sw_preset_medium', 'medium'), value: 'medium' },
        { label: tOr(t, 'config.sw_preset_slow', 'slow'), value: 'slow' },
        { label: tOr(t, 'config.sw_preset_slower', 'slower'), value: 'slower' },
        { label: tOr(t, 'config.sw_preset_veryslow', 'veryslow'), value: 'veryslow' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_tune': {
      const options = [
        { label: tOr(t, 'config.sw_tune_film', 'film'), value: 'film' },
        { label: tOr(t, 'config.sw_tune_animation', 'animation'), value: 'animation' },
        { label: tOr(t, 'config.sw_tune_grain', 'grain'), value: 'grain' },
        { label: tOr(t, 'config.sw_tune_stillimage', 'stillimage'), value: 'stillimage' },
        { label: tOr(t, 'config.sw_tune_fastdecode', 'fastdecode'), value: 'fastdecode' },
        { label: tOr(t, 'config.sw_tune_zerolatency', 'zerolatency'), value: 'zerolatency' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'frame_limiter_provider': {
      const options = [
        { label: tOr(t, 'frameLimiter.provider.auto', 'Auto'), value: 'auto' },
        { label: tOr(t, 'frameLimiter.provider.rtss', 'RTSS'), value: 'rtss' },
        {
          label: tOr(t, 'frameLimiter.provider.nvcp', 'NVIDIA Control Panel'),
          value: 'nvidia-control-panel',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'rtss_frame_limit_type': {
      const options = [
        { label: tOr(t, 'frameLimiter.syncLimiter.keep', 'Keep'), value: '' },
        { label: tOr(t, 'frameLimiter.syncLimiter.async', 'Async'), value: 'async' },
        {
          label: tOr(t, 'frameLimiter.syncLimiter.front', 'Front edge sync'),
          value: 'front edge sync',
        },
        {
          label: tOr(t, 'frameLimiter.syncLimiter.back', 'Back edge sync'),
          value: 'back edge sync',
        },
        {
          label: tOr(t, 'frameLimiter.syncLimiter.reflex', 'NVIDIA Reflex'),
          value: 'nvidia reflex',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_configuration_option': {
      const options = [
        { label: tOr(t, '_common.disabled', 'Disabled'), value: 'disabled' },
        { label: tOr(t, 'config.dd_config_verify_only', 'Verify only'), value: 'verify_only' },
        {
          label: tOr(t, 'config.dd_config_ensure_active', 'Ensure active'),
          value: 'ensure_active',
        },
        {
          label: tOr(t, 'config.dd_config_ensure_primary', 'Ensure primary'),
          value: 'ensure_primary',
        },
        {
          label: tOr(t, 'config.dd_config_ensure_only_display', 'Ensure only display'),
          value: 'ensure_only_display',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_resolution_option': {
      const options = [
        { label: tOr(t, 'config.dd_resolution_option_disabled', 'Disabled'), value: 'disabled' },
        { label: tOr(t, 'config.dd_resolution_option_auto', 'Auto'), value: 'auto' },
        { label: tOr(t, 'config.dd_resolution_option_manual', 'Manual'), value: 'manual' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_refresh_rate_option': {
      const options = [
        {
          label: tOr(t, 'config.dd_refresh_rate_option_disabled', 'Disabled'),
          value: 'disabled',
        },
        { label: tOr(t, 'config.dd_refresh_rate_option_auto', 'Auto'), value: 'auto' },
        { label: tOr(t, 'config.dd_refresh_rate_option_manual', 'Manual'), value: 'manual' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_hdr_option': {
      const options = [
        { label: tOr(t, 'config.dd_hdr_option_disabled', 'Disabled'), value: 'disabled' },
        { label: tOr(t, 'config.dd_hdr_option_auto', 'Auto'), value: 'auto' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_mode': {
      const options = [
        {
          label: tOr(t, 'config.virtual_display_mode_disabled', 'Disabled'),
          value: 'disabled',
        },
        {
          label: tOr(t, 'config.virtual_display_mode_per_client', 'Per client'),
          value: 'per_client',
        },
        { label: tOr(t, 'config.virtual_display_mode_shared', 'Shared'), value: 'shared' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_layout': {
      const options = [
        {
          label: tOr(t, 'config.virtual_display_layout_exclusive', 'Exclusive'),
          value: 'exclusive',
        },
        { label: tOr(t, 'config.virtual_display_layout_extended', 'Extended'), value: 'extended' },
        {
          label: tOr(t, 'config.virtual_display_layout_extended_primary', 'Extended (primary)'),
          value: 'extended_primary',
        },
        {
          label: tOr(t, 'config.virtual_display_layout_extended_isolated', 'Extended (isolated)'),
          value: 'extended_isolated',
        },
        {
          label: tOr(
            t,
            'config.virtual_display_layout_extended_primary_isolated',
            'Extended (primary isolated)',
          ),
          value: 'extended_primary_isolated',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    default:
      return [];
  }
}

export function buildOverrideOptionsText(options: OverrideSelectOption[]): string {
  if (options.length === 0) return '';
  return options
    .map((o) => `${o.label || ''} ${String(o.value ?? '')}`.trim())
    .filter(Boolean)
    .join(' | ');
}
