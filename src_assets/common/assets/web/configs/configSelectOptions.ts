export type ConfigSelectOption = { label: string; value: string | number; disabled?: boolean };

export type ConfigSelectOptionsContext = {
  t: (key: string) => string;
  platform: string;
  metadata?: any;
  currentValue?: unknown;
};

export function translateOr(t: (key: string) => string, key: string, fallback: string): string {
  const value = t(key);
  if (!value || value === key) return fallback;
  return value;
}

function isSelectValue(value: unknown): value is string | number {
  return typeof value === 'string' || (typeof value === 'number' && Number.isFinite(value));
}

function ensureIncludesCurrentValue(
  options: ConfigSelectOption[],
  currentValue: unknown,
): ConfigSelectOption[] {
  if (!isSelectValue(currentValue)) return options;
  if (options.some((option) => option.value === currentValue)) return options;
  return options.concat([{ label: String(currentValue), value: currentValue }]);
}

function gpuFlags(metadata: any) {
  const gpus = Array.isArray(metadata?.gpus) ? metadata.gpus : [];
  const hasVendor = (vendorId: number) =>
    gpus.some((gpu: any) => Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0) === vendorId);

  const metaNvidia = metadata?.has_nvidia_gpu;
  const metaIntel = metadata?.has_intel_gpu;
  const metaAmd = metadata?.has_amd_gpu;

  const hasNvidia =
    typeof metaNvidia === 'boolean' ? metaNvidia : gpus.length ? hasVendor(0x10de) : true;
  const hasIntel =
    typeof metaIntel === 'boolean' ? metaIntel : gpus.length ? hasVendor(0x8086) : true;
  const hasAmd =
    typeof metaAmd === 'boolean'
      ? metaAmd
      : gpus.length
        ? gpus.some((gpu: any) => {
            const vendor = Number(gpu?.vendor_id ?? gpu?.vendorId ?? 0);
            return vendor === 0x1002 || vendor === 0x1022;
          })
        : true;

  return { hasNvidia, hasIntel, hasAmd };
}

const localeOptions: ConfigSelectOption[] = [
  { label: 'Български (Bulgarian)', value: 'bg' },
  { label: 'Čeština (Czech)', value: 'cs' },
  { label: 'Deutsch (German)', value: 'de' },
  { label: 'English', value: 'en' },
  { label: 'English, UK', value: 'en_GB' },
  { label: 'English, US', value: 'en_US' },
  { label: 'Español (Spanish)', value: 'es' },
  { label: 'Français (French)', value: 'fr' },
  { label: 'Magyar (Hungarian)', value: 'hu' },
  { label: 'Italiano (Italian)', value: 'it' },
  { label: '日本語 (Japanese)', value: 'ja' },
  { label: '한국어 (Korean)', value: 'ko' },
  { label: 'Polski (Polish)', value: 'pl' },
  { label: 'Português (Portuguese)', value: 'pt' },
  { label: 'Português, Brasileiro (Portuguese, Brazilian)', value: 'pt_BR' },
  { label: 'Русский (Russian)', value: 'ru' },
  { label: 'svenska (Swedish)', value: 'sv' },
  { label: 'Türkçe (Turkish)', value: 'tr' },
  { label: 'Українська (Ukrainian)', value: 'uk' },
  { label: 'Tiếng Việt (Vietnamese)', value: 'vi' },
  { label: '简体中文 (Chinese Simplified)', value: 'zh' },
  { label: '繁體中文 (Chinese Traditional)', value: 'zh_TW' },
];

export function getConfigSelectOptions(
  key: string,
  ctx: ConfigSelectOptionsContext,
): ConfigSelectOption[] {
  const platform = String(ctx.platform || '').toLowerCase();
  const { t } = ctx;

  switch (key) {
    case 'locale':
      return ensureIncludesCurrentValue(localeOptions, ctx.currentValue);
    case 'min_log_level': {
      const options = [0, 1, 2, 3, 4, 5, 6].map((value) => ({
        label: translateOr(t, `config.min_log_level_${value}`, String(value)),
        value,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'address_family': {
      const options = [
        { label: translateOr(t, 'config.address_family_ipv4', 'IPv4'), value: 'ipv4' },
        { label: translateOr(t, 'config.address_family_both', 'Both'), value: 'both' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'origin_web_ui_allowed': {
      const options = [
        { label: translateOr(t, 'config.origin_web_ui_allowed_pc', 'PC'), value: 'pc' },
        { label: translateOr(t, 'config.origin_web_ui_allowed_lan', 'LAN'), value: 'lan' },
        { label: translateOr(t, 'config.origin_web_ui_allowed_wan', 'WAN'), value: 'wan' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'lan_encryption_mode': {
      const options = [
        { label: translateOr(t, '_common.disabled_def', 'Disabled (default)'), value: 0 },
        {
          label: translateOr(t, 'config.lan_encryption_mode_1', 'Opportunistic'),
          value: 1,
        },
        { label: translateOr(t, 'config.lan_encryption_mode_2', 'Forced'), value: 2 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'wan_encryption_mode': {
      const options = [
        { label: translateOr(t, '_common.disabled', 'Disabled'), value: 0 },
        {
          label: translateOr(t, 'config.wan_encryption_mode_1', 'Opportunistic'),
          value: 1,
        },
        { label: translateOr(t, 'config.wan_encryption_mode_2', 'Forced'), value: 2 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'hevc_mode': {
      const options = [0, 1, 2, 3].map((value) => ({
        label: translateOr(t, `config.hevc_mode_${value}`, String(value)),
        value,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'av1_mode': {
      const options = [0, 1, 2, 3].map((value) => ({
        label: translateOr(t, `config.av1_mode_${value}`, String(value)),
        value,
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
        freebsd: ['switch', 'xone'],
        linux: ['ds5', 'xone', 'switch', 'x360'],
        windows: ['x360', 'ds4'],
      };
      const fallbackOrder = ['x360', 'ds5', 'ds4'];

      const options: ConfigSelectOption[] = [
        { label: translateOr(t, '_common.auto', 'Auto'), value: 'auto' },
      ];
      const seen = new Set<string>(options.map((option) => String(option.value)));

      const addOption = (value: string | undefined) => {
        if (!value || seen.has(value)) return;
        const labelKey = labelMap[value] || `config.gamepad_${value}`;
        options.push({ label: translateOr(t, labelKey, value), value });
        seen.add(value);
      };

      const platformOrder = prioritizedByPlatform[platform] ?? fallbackOrder;
      platformOrder.forEach(addOption);
      if (typeof ctx.currentValue === 'string' && ctx.currentValue !== 'auto') {
        addOption(ctx.currentValue);
      }
      return options;
    }
    case 'capture': {
      const options: ConfigSelectOption[] = [
        { label: translateOr(t, '_common.autodetect', 'Autodetect'), value: '' },
      ];
      if (platform === 'windows') {
        options.push(
          { label: 'Windows Graphics Capture (variable)', value: 'wgc' },
          { label: 'Windows Graphics Capture (constant)', value: 'wgcc' },
          { label: 'Desktop Duplication API', value: 'ddx' },
        );
      } else if (platform === 'linux') {
        options.push(
          { label: 'NvFBC', value: 'nvfbc' },
          { label: 'wlroots', value: 'wlr' },
          { label: 'KMS', value: 'kms' },
          { label: 'X11', value: 'x11' },
        );
      }
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'encoder': {
      const options: ConfigSelectOption[] = [
        { label: translateOr(t, '_common.autodetect', 'Autodetect'), value: '' },
      ];
      const { hasNvidia, hasIntel, hasAmd } = gpuFlags(ctx.metadata);
      if (platform === 'windows') {
        if (hasNvidia) options.push({ label: 'NVIDIA NVENC', value: 'nvenc' });
        if (hasIntel) options.push({ label: 'Intel QuickSync', value: 'quicksync' });
        if (hasAmd) options.push({ label: 'AMD AMF/VCE', value: 'amdvce' });
      } else if (platform === 'linux') {
        options.push(
          { label: 'NVIDIA NVENC', value: 'nvenc' },
          { label: 'VA-API', value: 'vaapi' },
        );
      } else if (platform === 'macos') {
        options.push({ label: 'VideoToolbox', value: 'videotoolbox' });
      }
      options.push({
        label: translateOr(t, 'config.encoder_software', 'Software'),
        value: 'software',
      });
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_preset': {
      const fallbackExtra: Record<1 | 4 | 7, string> = {
        1: '(fastest, default)',
        4: '(balanced quality)',
        7: '(slowest)',
      };
      const presetExtra = (id: 1 | 4 | 7) => {
        const labelKey = `config.nvenc_preset_${id}`;
        const translated = t(labelKey);
        return translated && translated !== labelKey ? translated : fallbackExtra[id];
      };

      const options: ConfigSelectOption[] = [
        { label: `P1 ${presetExtra(1)}`.trim(), value: 1 },
        { label: 'P2', value: 2 },
        { label: 'P3', value: 3 },
        { label: `P4 ${presetExtra(4)}`.trim(), value: 4 },
        { label: 'P5', value: 5 },
        { label: 'P6', value: 6 },
        { label: `P7 ${presetExtra(7)}`.trim(), value: 7 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_twopass': {
      const options = [
        {
          label: translateOr(t, 'config.nvenc_twopass_disabled', 'Disabled'),
          value: 'disabled',
        },
        {
          label: translateOr(t, 'config.nvenc_twopass_quarter_res', 'Quarter res'),
          value: 'quarter_res',
        },
        {
          label: translateOr(t, 'config.nvenc_twopass_full_res', 'Full res'),
          value: 'full_res',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_split_encode':
    case 'nvenc_force_split_encode': {
      const options = [
        { label: translateOr(t, '_common.auto', 'Auto'), value: 'auto' },
        { label: translateOr(t, '_common.enabled', 'Enabled'), value: 'enabled' },
        { label: translateOr(t, '_common.disabled', 'Disabled'), value: 'disabled' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_preset': {
      const options = [
        { label: translateOr(t, 'config.qsv_preset_veryfast', 'veryfast'), value: 'veryfast' },
        { label: translateOr(t, 'config.qsv_preset_faster', 'faster'), value: 'faster' },
        { label: translateOr(t, 'config.qsv_preset_fast', 'fast'), value: 'fast' },
        { label: translateOr(t, 'config.qsv_preset_medium', 'medium'), value: 'medium' },
        { label: translateOr(t, 'config.qsv_preset_slow', 'slow'), value: 'slow' },
        { label: translateOr(t, 'config.qsv_preset_slower', 'slower'), value: 'slower' },
        { label: translateOr(t, 'config.qsv_preset_slowest', 'slowest'), value: 'slowest' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_coder':
    case 'amd_coder':
    case 'vt_coder': {
      const options = [
        { label: translateOr(t, 'config.ffmpeg_auto', 'Auto'), value: 'auto' },
        { label: translateOr(t, 'config.coder_cabac', 'CABAC'), value: 'cabac' },
        { label: translateOr(t, 'config.coder_cavlc', 'CAVLC'), value: 'cavlc' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_usage': {
      const options = [
        {
          label: translateOr(t, 'config.amd_usage_transcoding', 'Transcoding'),
          value: 'transcoding',
        },
        { label: translateOr(t, 'config.amd_usage_webcam', 'Webcam'), value: 'webcam' },
        {
          label: translateOr(
            t,
            'config.amd_usage_lowlatency_high_quality',
            'Low latency (high quality)',
          ),
          value: 'lowlatency_high_quality',
        },
        {
          label: translateOr(t, 'config.amd_usage_lowlatency', 'Low latency'),
          value: 'lowlatency',
        },
        {
          label: translateOr(t, 'config.amd_usage_ultralowlatency', 'Ultra low latency'),
          value: 'ultralowlatency',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_rc': {
      const options = [
        { label: translateOr(t, 'config.amd_rc_cbr', 'CBR'), value: 'cbr' },
        { label: translateOr(t, 'config.amd_rc_cqp', 'CQP'), value: 'cqp' },
        {
          label: translateOr(t, 'config.amd_rc_vbr_latency', 'VBR (latency)'),
          value: 'vbr_latency',
        },
        { label: translateOr(t, 'config.amd_rc_vbr_peak', 'VBR (peak)'), value: 'vbr_peak' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_quality': {
      const options = [
        { label: translateOr(t, 'config.amd_quality_speed', 'Speed'), value: 'speed' },
        { label: translateOr(t, 'config.amd_quality_balanced', 'Balanced'), value: 'balanced' },
        { label: translateOr(t, 'config.amd_quality_quality', 'Quality'), value: 'quality' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'vt_software': {
      const options = [
        { label: translateOr(t, '_common.auto', 'Auto'), value: 'auto' },
        { label: translateOr(t, '_common.disabled', 'Disabled'), value: 'disabled' },
        { label: translateOr(t, 'config.vt_software_allowed', 'Allowed'), value: 'allowed' },
        { label: translateOr(t, 'config.vt_software_forced', 'Forced'), value: 'forced' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_preset': {
      const options = [
        { label: translateOr(t, 'config.sw_preset_ultrafast', 'ultrafast'), value: 'ultrafast' },
        { label: translateOr(t, 'config.sw_preset_superfast', 'superfast'), value: 'superfast' },
        { label: translateOr(t, 'config.sw_preset_veryfast', 'veryfast'), value: 'veryfast' },
        { label: translateOr(t, 'config.sw_preset_faster', 'faster'), value: 'faster' },
        { label: translateOr(t, 'config.sw_preset_fast', 'fast'), value: 'fast' },
        { label: translateOr(t, 'config.sw_preset_medium', 'medium'), value: 'medium' },
        { label: translateOr(t, 'config.sw_preset_slow', 'slow'), value: 'slow' },
        { label: translateOr(t, 'config.sw_preset_slower', 'slower'), value: 'slower' },
        { label: translateOr(t, 'config.sw_preset_veryslow', 'veryslow'), value: 'veryslow' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_tune': {
      const options = [
        { label: translateOr(t, 'config.sw_tune_film', 'film'), value: 'film' },
        { label: translateOr(t, 'config.sw_tune_animation', 'animation'), value: 'animation' },
        { label: translateOr(t, 'config.sw_tune_grain', 'grain'), value: 'grain' },
        { label: translateOr(t, 'config.sw_tune_stillimage', 'stillimage'), value: 'stillimage' },
        { label: translateOr(t, 'config.sw_tune_fastdecode', 'fastdecode'), value: 'fastdecode' },
        {
          label: translateOr(t, 'config.sw_tune_zerolatency', 'zerolatency'),
          value: 'zerolatency',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'frame_limiter_provider': {
      const options = [
        { label: translateOr(t, 'frameLimiter.provider.auto', 'Auto'), value: 'auto' },
        { label: translateOr(t, 'frameLimiter.provider.rtss', 'RTSS'), value: 'rtss' },
        {
          label: translateOr(t, 'frameLimiter.provider.nvcp', 'NVIDIA Control Panel'),
          value: 'nvidia-control-panel',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'rtss_frame_limit_type': {
      const options = [
        { label: translateOr(t, 'frameLimiter.syncLimiter.keep', 'Keep'), value: '' },
        { label: translateOr(t, 'frameLimiter.syncLimiter.async', 'Async'), value: 'async' },
        {
          label: translateOr(t, 'frameLimiter.syncLimiter.front', 'Front edge sync'),
          value: 'front edge sync',
        },
        {
          label: translateOr(t, 'frameLimiter.syncLimiter.back', 'Back edge sync'),
          value: 'back edge sync',
        },
        {
          label: translateOr(t, 'frameLimiter.syncLimiter.reflex', 'NVIDIA Reflex'),
          value: 'nvidia reflex',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_configuration_option': {
      const options = [
        { label: translateOr(t, '_common.disabled', 'Disabled'), value: 'disabled' },
        {
          label: translateOr(t, 'config.dd_config_verify_only', 'Verify only'),
          value: 'verify_only',
        },
        {
          label: translateOr(t, 'config.dd_config_ensure_active', 'Ensure active'),
          value: 'ensure_active',
        },
        {
          label: translateOr(t, 'config.dd_config_ensure_primary', 'Ensure primary'),
          value: 'ensure_primary',
        },
        {
          label: translateOr(t, 'config.dd_config_ensure_only_display', 'Ensure only display'),
          value: 'ensure_only_display',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_resolution_option': {
      const options = [
        {
          label: translateOr(t, 'config.dd_resolution_option_disabled', 'Disabled'),
          value: 'disabled',
        },
        { label: translateOr(t, 'config.dd_resolution_option_auto', 'Auto'), value: 'auto' },
        { label: translateOr(t, 'config.dd_resolution_option_manual', 'Manual'), value: 'manual' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_refresh_rate_option': {
      const options = [
        {
          label: translateOr(t, 'config.dd_refresh_rate_option_disabled', 'Disabled'),
          value: 'disabled',
        },
        { label: translateOr(t, 'config.dd_refresh_rate_option_auto', 'Auto'), value: 'auto' },
        {
          label: translateOr(t, 'config.dd_refresh_rate_option_manual', 'Manual'),
          value: 'manual',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_hdr_option': {
      const options = [
        { label: translateOr(t, 'config.dd_hdr_option_disabled', 'Disabled'), value: 'disabled' },
        { label: translateOr(t, 'config.dd_hdr_option_auto', 'Auto'), value: 'auto' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_mode': {
      const options = [
        {
          label: translateOr(t, 'config.virtual_display_mode_disabled', 'Disabled'),
          value: 'disabled',
        },
        {
          label: translateOr(t, 'config.virtual_display_mode_per_client', 'Per client'),
          value: 'per_client',
        },
        { label: translateOr(t, 'config.virtual_display_mode_shared', 'Shared'), value: 'shared' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_layout': {
      const options = [
        {
          label: translateOr(t, 'config.virtual_display_layout_exclusive', 'Exclusive'),
          value: 'exclusive',
        },
        {
          label: translateOr(t, 'config.virtual_display_layout_extended', 'Extended'),
          value: 'extended',
        },
        {
          label: translateOr(
            t,
            'config.virtual_display_layout_extended_primary',
            'Extended (primary)',
          ),
          value: 'extended_primary',
        },
        {
          label: translateOr(
            t,
            'config.virtual_display_layout_extended_isolated',
            'Extended (isolated)',
          ),
          value: 'extended_isolated',
        },
        {
          label: translateOr(
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

export function buildConfigOptionsText(options: ConfigSelectOption[]): string {
  if (options.length === 0) return '';
  return options
    .map((option) => `${option.label || ''} ${String(option.value ?? '')}`.trim())
    .filter(Boolean)
    .join(' | ');
}
