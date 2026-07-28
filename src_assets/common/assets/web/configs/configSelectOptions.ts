export type ConfigSelectOption = { label: string; value: string | number; disabled?: boolean };

export type ConfigSelectOptionsContext = {
  t: (key: string) => string;
  platform: string;
  metadata?: any;
  currentValue?: unknown;
};

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
  { label: 'Български', value: 'bg' },
  { label: 'Čeština', value: 'cs' },
  { label: 'Deutsch', value: 'de' },
  { label: 'English', value: 'en' },
  { label: 'English (UK)', value: 'en_GB' },
  { label: 'English (US)', value: 'en_US' },
  { label: 'Español', value: 'es' },
  { label: 'Français', value: 'fr' },
  { label: 'Magyar', value: 'hu' },
  { label: 'Italiano', value: 'it' },
  { label: '日本語', value: 'ja' },
  { label: '한국어', value: 'ko' },
  { label: 'Polski', value: 'pl' },
  { label: 'Português', value: 'pt' },
  { label: 'Português (Brasil)', value: 'pt_BR' },
  { label: 'Русский', value: 'ru' },
  { label: 'Svenska', value: 'sv' },
  { label: 'Türkçe', value: 'tr' },
  { label: 'Українська', value: 'uk' },
  { label: 'Tiếng Việt', value: 'vi' },
  { label: '简体中文', value: 'zh' },
  { label: '繁體中文', value: 'zh_TW' },
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
        label: t(`config.min_log_level_${value}`),
        value,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'address_family': {
      const options = [
        { label: t('config.address_family_ipv4'), value: 'ipv4' },
        { label: t('config.address_family_both'), value: 'both' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'origin_web_ui_allowed': {
      const options = [
        { label: t('config.origin_web_ui_allowed_pc'), value: 'pc' },
        { label: t('config.origin_web_ui_allowed_lan'), value: 'lan' },
        { label: t('config.origin_web_ui_allowed_wan'), value: 'wan' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'lan_encryption_mode': {
      const options = [
        { label: t('_common.disabled_def'), value: 0 },
        {
          label: t('config.lan_encryption_mode_1'),
          value: 1,
        },
        { label: t('config.lan_encryption_mode_2'), value: 2 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'wan_encryption_mode': {
      const options = [
        { label: t('_common.disabled'), value: 0 },
        {
          label: t('config.wan_encryption_mode_1'),
          value: 1,
        },
        { label: t('config.wan_encryption_mode_2'), value: 2 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'video_max_batch_size_kb': {
      const options = [
        { label: `64 KiB ${t('_common.default_parenthetical')}`, value: 64 },
        { label: '32 KiB', value: 32 },
        { label: '16 KiB', value: 16 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'hevc_mode': {
      const options = [0, 1, 2, 3].map((value) => ({
        label: t(`config.hevc_mode_${value}`),
        value,
      }));
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'av1_mode': {
      const options = [0, 1, 2, 3].map((value) => ({
        label: t(`config.av1_mode_${value}`),
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

      const options: ConfigSelectOption[] = [{ label: t('_common.auto'), value: 'auto' }];
      const seen = new Set<string>(options.map((option) => String(option.value)));

      const addOption = (value: string | undefined) => {
        if (!value || seen.has(value)) return;
        const labelKey = labelMap[value] || `config.gamepad_${value}`;
        const translated = t(labelKey);
        options.push({ label: translated && translated !== labelKey ? translated : value, value });
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
      const options: ConfigSelectOption[] = [{ label: t('_common.autodetect'), value: '' }];
      if (platform === 'windows') {
        options.push(
          { label: t('config.capture_wgc_auto'), value: 'wgc' },
          { label: t('config.capture_wgc_constant'), value: 'wgcc' },
          { label: t('config.capture_ddx_legacy'), value: 'ddx' },
        );
      } else if (platform === 'linux') {
        options.push(
          { label: 'NvFBC', value: 'nvfbc' },
          { label: 'KWin', value: 'kwin' },
          { label: 'wlroots', value: 'wlr' },
          { label: 'KMS', value: 'kms' },
          { label: 'X11', value: 'x11' },
        );
      }
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'encoder': {
      const options: ConfigSelectOption[] = [{ label: t('_common.autodetect'), value: '' }];
      const { hasNvidia, hasIntel, hasAmd } = gpuFlags(ctx.metadata);
      if (platform === 'windows') {
        if (hasNvidia) options.push({ label: 'NVIDIA NVENC', value: 'nvenc' });
        if (hasIntel) options.push({ label: 'Intel QuickSync', value: 'quicksync' });
        if (hasAmd) options.push({ label: 'AMD AMF/VCE', value: 'amdvce' });
        // Explicit rollback to the FFmpeg AMF path — never probed automatically,
        // so it must stay selectable here.
        if (hasAmd) options.push({ label: 'AMD AMF/VCE (legacy FFmpeg)', value: 'amdvce_legacy' });
      } else if (platform === 'linux') {
        options.push(
          { label: 'NVIDIA NVENC', value: 'nvenc' },
          { label: 'Vulkan', value: 'vulkan' },
          { label: 'VA-API', value: 'vaapi' },
        );
      } else if (platform === 'macos') {
        options.push({ label: 'VideoToolbox', value: 'videotoolbox' });
      }
      options.push({
        label: t('config.encoder_software'),
        value: 'software',
      });
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_preset': {
      const presetExtra = (id: 1 | 4 | 7) => t(`config.nvenc_preset_${id}`);

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
          label: t('config.nvenc_twopass_disabled'),
          value: 'disabled',
        },
        {
          label: t('config.nvenc_twopass_quarter_res'),
          value: 'quarter_res',
        },
        {
          label: t('config.nvenc_twopass_full_res'),
          value: 'full_res',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'nvenc_split_encode':
    case 'nvenc_force_split_encode': {
      const options = [
        { label: t('_common.auto'), value: 'auto' },
        { label: t('_common.enabled'), value: 'enabled' },
        { label: t('_common.disabled'), value: 'disabled' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'vk_tune': {
      const options = [
        { label: t('_common.auto'), value: 0 },
        { label: t('config.vk_tune_hq'), value: 1 },
        { label: t('config.vk_tune_ll'), value: 2 },
        { label: t('config.vk_tune_ull'), value: 3 },
        { label: t('config.vk_tune_lossless'), value: 4 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'vk_rc_mode': {
      const options = [
        { label: t('_common.auto'), value: 0 },
        { label: t('config.vk_rc_cqp'), value: 1 },
        { label: t('config.vk_rc_cbr'), value: 2 },
        { label: t('config.vk_rc_vbr'), value: 4 },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_preset': {
      const options = [
        { label: t('config.qsv_preset_veryfast'), value: 'veryfast' },
        { label: t('config.qsv_preset_faster'), value: 'faster' },
        { label: t('config.qsv_preset_fast'), value: 'fast' },
        { label: t('config.qsv_preset_medium'), value: 'medium' },
        { label: t('config.qsv_preset_slow'), value: 'slow' },
        { label: t('config.qsv_preset_slower'), value: 'slower' },
        { label: t('config.qsv_preset_slowest'), value: 'slowest' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'qsv_coder':
    case 'amd_coder':
    case 'vt_coder': {
      const options = [
        { label: t('config.ffmpeg_auto'), value: 'auto' },
        { label: t('config.coder_cabac'), value: 'cabac' },
        { label: t('config.coder_cavlc'), value: 'cavlc' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_usage': {
      const options = [
        {
          label: t('config.amd_usage_transcoding'),
          value: 'transcoding',
        },
        { label: t('config.amd_usage_webcam'), value: 'webcam' },
        {
          label: t('config.amd_usage_lowlatency_high_quality'),
          value: 'lowlatency_high_quality',
        },
        {
          label: t('config.amd_usage_lowlatency'),
          value: 'lowlatency',
        },
        {
          label: t('config.amd_usage_ultralowlatency'),
          value: 'ultralowlatency',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_rc': {
      const options = [
        { label: t('config.amd_rc_cbr'), value: 'cbr' },
        { label: t('config.amd_rc_cqp'), value: 'cqp' },
        {
          label: t('config.amd_rc_vbr_latency'),
          value: 'vbr_latency',
        },
        { label: t('config.amd_rc_vbr_peak'), value: 'vbr_peak' },
        { label: t('config.amd_rc_qvbr'), value: 'qvbr' },
        { label: t('config.amd_rc_hqvbr'), value: 'hqvbr' },
        { label: t('config.amd_rc_hqcbr'), value: 'hqcbr' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_quality': {
      const options = [
        { label: t('config.amd_opt_auto'), value: 'auto' },
        { label: t('config.amd_quality_speed'), value: 'speed' },
        { label: t('config.amd_quality_balanced'), value: 'balanced' },
        { label: t('config.amd_quality_quality'), value: 'quality' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_vbaq': {
      const options = [
        { label: t('config.amd_opt_auto'), value: 'auto' },
        { label: t('config.amd_opt_enabled'), value: 'enabled' },
        { label: t('config.amd_opt_disabled'), value: 'disabled' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_av1_screen_content': {
      const options = [
        { label: t('config.amd_opt_auto'), value: 'auto' },
        { label: t('config.amd_opt_enabled'), value: 'enabled' },
        { label: t('config.amd_opt_disabled'), value: 'disabled' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'amd_av1_latency_mode': {
      const options = [
        { label: t('config.amd_opt_auto'), value: 'auto' },
        { label: t('config.amd_av1_latency_none'), value: 'none' },
        {
          label: t('config.amd_av1_latency_power_saving'),
          value: 'power_saving',
        },
        {
          label: t('config.amd_av1_latency_realtime'),
          value: 'realtime',
        },
        {
          label: t('config.amd_av1_latency_lowest'),
          value: 'lowest',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'vt_software': {
      const options = [
        { label: t('_common.auto'), value: 'auto' },
        { label: t('_common.disabled'), value: 'disabled' },
        { label: t('config.vt_software_allowed'), value: 'allowed' },
        { label: t('config.vt_software_forced'), value: 'forced' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_preset': {
      const options = [
        { label: t('config.sw_preset_ultrafast'), value: 'ultrafast' },
        { label: t('config.sw_preset_superfast'), value: 'superfast' },
        { label: t('config.sw_preset_veryfast'), value: 'veryfast' },
        { label: t('config.sw_preset_faster'), value: 'faster' },
        { label: t('config.sw_preset_fast'), value: 'fast' },
        { label: t('config.sw_preset_medium'), value: 'medium' },
        { label: t('config.sw_preset_slow'), value: 'slow' },
        { label: t('config.sw_preset_slower'), value: 'slower' },
        { label: t('config.sw_preset_veryslow'), value: 'veryslow' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'sw_tune': {
      const options = [
        { label: t('config.sw_tune_film'), value: 'film' },
        { label: t('config.sw_tune_animation'), value: 'animation' },
        { label: t('config.sw_tune_grain'), value: 'grain' },
        { label: t('config.sw_tune_stillimage'), value: 'stillimage' },
        { label: t('config.sw_tune_fastdecode'), value: 'fastdecode' },
        {
          label: t('config.sw_tune_zerolatency'),
          value: 'zerolatency',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'frame_limiter_provider': {
      const options = [
        { label: t('frameLimiter.provider.auto'), value: 'auto' },
        { label: t('frameLimiter.provider.rtss'), value: 'rtss' },
        {
          label: t('frameLimiter.provider.nvcp'),
          value: 'nvidia-control-panel',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'frame_limiter_auto_virtual_framegen': {
      const options = [
        {
          label: t('frameLimiter.virtual.modeEnabled'),
          value: 'enabled',
        },
        {
          label: t('frameLimiter.virtual.modeDisabled'),
          value: 'disabled',
        },
        {
          label: t('frameLimiter.virtual.modeLegacy'),
          value: 'legacy',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'rtss_frame_limit_type': {
      const options = [
        { label: t('frameLimiter.syncLimiter.keep'), value: '' },
        { label: t('frameLimiter.syncLimiter.async'), value: 'async' },
        {
          label: t('frameLimiter.syncLimiter.front'),
          value: 'front edge sync',
        },
        {
          label: t('frameLimiter.syncLimiter.back'),
          value: 'back edge sync',
        },
        {
          label: t('frameLimiter.syncLimiter.reflex'),
          value: 'nvidia reflex',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_configuration_option': {
      const options = [
        { label: t('_common.disabled'), value: 'disabled' },
        {
          label: t('config.dd_config_verify_only'),
          value: 'verify_only',
        },
        {
          label: t('config.dd_config_ensure_active'),
          value: 'ensure_active',
        },
        {
          label: t('config.dd_config_ensure_primary'),
          value: 'ensure_primary',
        },
        {
          label: t('config.dd_config_ensure_only_display'),
          value: 'ensure_only_display',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_resolution_option': {
      const options = [
        {
          label: t('config.dd_resolution_option_disabled'),
          value: 'disabled',
        },
        { label: t('config.dd_resolution_option_auto'), value: 'auto' },
        { label: t('config.dd_resolution_option_manual'), value: 'manual' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_refresh_rate_option': {
      const options = [
        {
          label: t('config.dd_refresh_rate_option_disabled'),
          value: 'disabled',
        },
        { label: t('config.dd_refresh_rate_option_auto'), value: 'auto' },
        {
          label: t('config.dd_refresh_rate_option_manual'),
          value: 'manual',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_hdr_option': {
      const options = [
        { label: t('config.dd_hdr_option_disabled'), value: 'disabled' },
        { label: t('config.dd_hdr_option_auto'), value: 'auto' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_mode': {
      const options = [
        {
          label: t('config.virtual_display_mode_disabled'),
          value: 'disabled',
        },
        {
          label: t('config.virtual_display_mode_per_client'),
          value: 'per_client',
        },
        { label: t('config.virtual_display_mode_shared'), value: 'shared' },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'virtual_display_layout': {
      const options = [
        {
          label: t('config.virtual_display_layout_exclusive'),
          value: 'exclusive',
        },
        {
          label: t('config.virtual_display_layout_extended'),
          value: 'extended',
        },
        {
          label: t('config.virtual_display_layout_extended_primary'),
          value: 'extended_primary',
        },
        {
          label: t('config.virtual_display_layout_extended_isolated'),
          value: 'extended_isolated',
        },
        {
          label: t('config.virtual_display_layout_extended_primary_isolated'),
          value: 'extended_primary_isolated',
        },
      ];
      return ensureIncludesCurrentValue(options, ctx.currentValue);
    }
    case 'dd_virtual_display_scale': {
      const options: ConfigSelectOption[] = [
        { label: t('config.virtual_display_scale_recommended'), value: -1 },
        { label: t('config.virtual_display_scale_auto'), value: 0 },
        ...[100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500].map((value) => ({
          label: `${value}%`,
          value,
        })),
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
