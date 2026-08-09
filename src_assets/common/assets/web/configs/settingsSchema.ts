export type SettingsFieldKind =
  | 'boolean'
  | 'number'
  | 'select'
  | 'duration'
  | 'text'
  | 'integration-path'
  | 'textarea'
  | 'mode-remapping'
  | 'display-recovery'
  | 'command-preparations';

export interface SettingsOption {
  labelKey: string;
  value: string;
}

export interface SettingsVisibility {
  key: string;
  equals?: string | boolean;
  notEquals?: string | boolean;
}

export interface SettingsField {
  key: string;
  kind: SettingsFieldKind;
  labelKey?: string;
  descriptionKey?: string;
  warningKey?: string;
  options?: SettingsOption[];
  min?: number;
  max?: number;
  step?: number;
  placeholderKey?: string;
  monospace?: boolean;
  restartRequired?: boolean;
  stacked?: boolean;
  recommended?: boolean;
  simple?: boolean;
  platform?: 'windows' | 'linux' | 'macos';
  visibleWhen?: SettingsVisibility;
  source?: 'gpu';
  encoderFamily?: 'nvidia' | 'intel' | 'amd';
  integration?: 'rtss' | 'lossless';
}

// Keep this list aligned with config::is_allowed_override_key(). The global
// settings schema also contains network, filesystem, identity, and updater
// fields that must never be offered as per-client runtime overrides.
export const clientOverrideableKeys = new Set([
  'controller',
  'gamepad',
  'ds4_back_as_touchpad_click',
  'motion_as_ds4',
  'touchpad_as_ds4',
  'back_button_timeout',
  'keyboard',
  'key_repeat_delay',
  'key_repeat_frequency',
  'always_send_scancodes',
  'key_rightalt_to_key_win',
  'mouse',
  'high_resolution_scrolling',
  'native_pen_touch',
  'keybindings',
  'ds5_inputtino_randomize_mac',
  'audio_sink',
  'virtual_sink',
  'stream_audio',
  'adapter_name',
  'adapter_pnp_id',
  'dd_configuration_option',
  'dd_resolution_option',
  'dd_manual_resolution',
  'dd_refresh_rate_option',
  'dd_manual_refresh_rate',
  'dd_hdr_option',
  'dd_hdr_request_override',
  'dd_config_revert_delay',
  'dd_config_revert_on_disconnect',
  'dd_paused_virtual_display_timeout_secs',
  'dd_always_restore_from_golden',
  'dd_display_helper_engine',
  'dd_snapshot_exclude_devices',
  'dd_snapshot_restore_hotkey',
  'dd_snapshot_restore_hotkey_modifiers',
  'dd_use_sunshine_virtual_display_driver',
  'dd_activate_virtual_display',
  'dd_virtual_display_scale',
  'dd_virtual_display_permanent_count',
  'dd_mode_remapping',
  'dd_wa_dummy_plug_hdr10',
  'max_bitrate',
  'minimum_fps_target',
  'fec_percentage',
  'video_max_batch_size_kb',
  'qp',
  'min_threads',
  'hevc_mode',
  'av1_mode',
  'capture',
  'encoder',
  'frame_limiter_enable',
  'frame_limiter_provider',
  'frame_limiter_fps_limit',
  'frame_limiter_auto_virtual_framegen',
  'rtss_frame_limit_type',
  'frame_limiter_disable_vsync',
  'nvenc_preset',
  'nvenc_twopass',
  'nvenc_spatial_aq',
  'nvenc_split_encode',
  'nvenc_vbv_increase',
  'nvenc_realtime_hags',
  'nvenc_latency_over_power',
  'nvenc_opengl_vulkan_on_dxgi',
  'nvenc_h264_cavlc',
  'qsv_preset',
  'qsv_coder',
  'qsv_slow_hevc',
  'amd_usage',
  'amd_rc',
  'amd_qvbr_quality_level',
  'amd_enforce_hrd',
  'amd_quality',
  'amd_preanalysis',
  'amd_vbaq',
  'amd_coder',
  'amd_ltr_frames',
  'amd_input_queue_size',
  'amd_smart_access_video',
  'amd_lowlatency_mode',
  'amd_high_motion_quality_boost',
  'amd_av1_screen_content',
  'amd_av1_latency_mode',
  'vt_coder',
  'vt_software',
  'vt_realtime',
  'vaapi_strict_rc_buffer',
  'vk_tune',
  'vk_rc_mode',
  'rtx_hdr',
  'rtx_hdr_force_sdr',
  'rtx_hdr_sdr_brightness',
  'rtx_hdr_contrast',
  'rtx_hdr_saturation',
  'rtx_hdr_middle_gray',
  'rtx_hdr_peak_brightness',
  'sw_preset',
  'sw_tune',
]);

export interface SettingsGroup {
  id: string;
  fields: SettingsField[];
  collapsed?: boolean;
  visibleWhen?: SettingsVisibility;
}

export interface SettingsCategory {
  id: string;
  groups: SettingsGroup[];
}

const option = (value: string, labelKey: string): SettingsOption => ({ value, labelKey });
const boolean = (key: string, extra: Partial<SettingsField> = {}): SettingsField => ({
  key,
  kind: 'boolean',
  ...extra,
});
const number = (key: string, extra: Partial<SettingsField> = {}): SettingsField => ({
  key,
  kind: 'number',
  ...extra,
});
const text = (key: string, extra: Partial<SettingsField> = {}): SettingsField => ({
  key,
  kind: 'text',
  ...extra,
});
const select = (
  key: string,
  options: SettingsOption[],
  extra: Partial<SettingsField> = {},
): SettingsField => ({ key, kind: 'select', options, ...extra });
const duration = (
  key: string,
  options: SettingsOption[],
  extra: Partial<SettingsField> = {},
): SettingsField => ({ key, kind: 'duration', options, ...extra });
const modeRemapping = (extra: Partial<SettingsField> = {}): SettingsField => ({
  key: 'dd_mode_remapping',
  kind: 'mode-remapping',
  stacked: true,
  ...extra,
});
const displayRecovery = (): SettingsField => ({
  key: 'dd_snapshot_restore_hotkey',
  kind: 'display-recovery',
  platform: 'windows',
  stacked: true,
});

const virtualDisplayOptions = [
  option('disabled', 'ui.settings.options.virtual_display_mode.physical'),
  option('per_client', 'ui.settings.options.virtual_display_mode.per_client'),
  option('shared', 'ui.settings.options.virtual_display_mode.shared'),
];

const virtualLayoutOptions = [
  option('exclusive', 'ui.settings.options.virtual_display_layout.exclusive'),
  option('extended', 'ui.settings.options.virtual_display_layout.extended'),
  option('extended_primary', 'ui.settings.options.virtual_display_layout.extended_primary'),
  option('extended_isolated', 'ui.settings.options.virtual_display_layout.extended_isolated'),
  option(
    'extended_primary_isolated',
    'ui.settings.options.virtual_display_layout.extended_primary_isolated',
  ),
];

const virtualScaleOptions = [
  option('-1', 'ui.settings.options.virtual_scale.recommended'),
  option('0', 'ui.settings.options.virtual_scale.preserve'),
  ...[100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500].map((scale) =>
    option(String(scale), 'ui.settings.options.virtual_scale.percent'),
  ),
];

const captureOptions = [
  option('', '_common.auto'),
  option('wgc', 'ui.settings.options.capture.wgc'),
  option('wgcc', 'ui.settings.options.capture.wgcc'),
  option('ddx', 'ui.settings.options.capture.ddx'),
];

const nvencPresetOptions = [
  option('1', 'ui.settings.options.nvenc_preset.p1'),
  option('2', 'ui.settings.options.nvenc_preset.p2'),
  option('3', 'ui.settings.options.nvenc_preset.p3'),
  option('4', 'ui.settings.options.nvenc_preset.p4'),
  option('5', 'ui.settings.options.nvenc_preset.p5'),
  option('6', 'ui.settings.options.nvenc_preset.p6'),
  option('7', 'ui.settings.options.nvenc_preset.p7'),
];

const nvencPresetField = (): SettingsField => select('nvenc_preset', nvencPresetOptions);

const qsvPresetOptions = [
  option('veryslow', 'ui.settings.options.qsv_preset.veryslow'),
  option('slower', 'ui.settings.options.qsv_preset.slower'),
  option('slow', 'ui.settings.options.qsv_preset.slow'),
  option('medium', 'ui.settings.options.qsv_preset.medium'),
  option('fast', 'ui.settings.options.qsv_preset.fast'),
  option('faster', 'ui.settings.options.qsv_preset.faster'),
  option('veryfast', 'ui.settings.options.qsv_preset.veryfast'),
];

const amdQualityOptions = [
  option('auto', 'ui.settings.options.amd_quality.auto'),
  option('speed', 'ui.settings.options.amd_quality.speed'),
  option('balanced', 'ui.settings.options.amd_quality.balanced'),
  option('quality', 'ui.settings.options.amd_quality.quality'),
];

const frameLimiterOptions = [
  option('auto', '_common.auto'),
  option('rtss', 'ui.settings.options.frame_limiter_provider.rtss'),
  option('nvidia-control-panel', 'ui.settings.options.frame_limiter_provider.nvidia'),
  option('none', 'ui.settings.options.frame_limiter_provider.none'),
];

const frameGenerationOptions = [
  option('enabled', 'ui.settings.options.frame_generation.automatic'),
  option('legacy', 'ui.settings.options.frame_generation.compatibility'),
  option('disabled', 'ui.settings.options.frame_generation.off'),
];

const integrationPath = (
  key: string,
  integration: SettingsField['integration'],
  extra: Partial<SettingsField> = {},
): SettingsField => ({
  key,
  kind: 'integration-path',
  integration,
  monospace: true,
  platform: 'windows',
  stacked: true,
  ...extra,
});

const everydayDisplayFields = (): SettingsField[] => [
  select('virtual_display_mode', virtualDisplayOptions, {
    labelKey: 'ui.settings.fields.virtual_display_mode.label',
    descriptionKey: 'ui.settings.fields.virtual_display_mode.description',
    recommended: true,
  }),
  select(
    'dd_configuration_option',
    [
      option('verify_only', 'ui.settings.options.display_preparation.verify_only'),
      option('ensure_active', 'ui.settings.options.display_preparation.ensure_active'),
      option('ensure_primary', 'ui.settings.options.display_preparation.ensure_primary'),
      option('ensure_only_display', 'ui.settings.options.display_preparation.ensure_only'),
      option('disabled', '_common.disabled'),
    ],
    {
      labelKey: 'config.dd_configuration_option',
      descriptionKey: 'ui.settings.fields.dd_configuration_option.description',
    },
  ),
  select('dd_resolution_option', [
    option('auto', 'ui.settings.options.resolution.auto'),
    option('disabled', 'ui.settings.options.resolution.preserve'),
    option('manual', 'ui.settings.options.resolution.manual'),
  ]),
  text('dd_manual_resolution', {
    placeholderKey: 'ui.settings.placeholders.resolution',
    visibleWhen: { key: 'dd_resolution_option', equals: 'manual' },
  }),
  select('dd_refresh_rate_option', [
    option('auto', 'ui.settings.options.refresh.auto'),
    option('prefer_highest', 'ui.settings.options.refresh.highest'),
    option('disabled', 'ui.settings.options.refresh.preserve'),
    option('manual', 'ui.settings.options.refresh.manual'),
  ]),
  number('dd_manual_refresh_rate', {
    min: 1,
    max: 1000,
    step: 0.001,
    placeholderKey: 'ui.settings.placeholders.refresh_rate',
    visibleWhen: { key: 'dd_refresh_rate_option', equals: 'manual' },
  }),
  select('dd_hdr_option', [
    option('auto', 'ui.settings.options.hdr.auto'),
    option('disabled', 'ui.settings.options.hdr.preserve'),
  ]),
  select('dd_hdr_request_override', [
    option('auto', 'ui.settings.options.hdr_request.auto'),
    option('force_on', 'ui.settings.options.hdr_request.force_on'),
    option('force_off', 'ui.settings.options.hdr_request.force_off'),
  ]),
];

const virtualDisplayCustomizationFields = (): SettingsField[] => [
  select('virtual_display_layout', virtualLayoutOptions, {
    labelKey: 'ui.settings.fields.virtual_display_layout.label',
    descriptionKey: 'ui.settings.fields.virtual_display_layout.description',
    visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
  }),
  select('dd_virtual_display_scale', virtualScaleOptions, {
    labelKey: 'ui.settings.fields.dd_virtual_display_scale.label',
    descriptionKey: 'ui.settings.fields.dd_virtual_display_scale.description',
    visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
  }),
];

const everydayPacingFields = (): SettingsField[] => [
  boolean('frame_limiter_enable', {
    labelKey: 'ui.settings.fields.frame_limiter_enable.label',
    descriptionKey: 'ui.settings.fields.frame_limiter_enable.description',
  }),
  select('frame_limiter_provider', frameLimiterOptions, {
    descriptionKey: 'ui.settings.fields.frame_limiter_provider.description',
  }),
  number('frame_limiter_fps_limit', {
    min: 0,
    max: 1000,
    step: 0.001,
    placeholderKey: 'ui.settings.placeholders.follow_client',
    descriptionKey: 'ui.settings.fields.frame_limiter_fps_limit.description',
  }),
  select('frame_limiter_auto_virtual_framegen', frameGenerationOptions, {
    visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
  }),
  boolean('frame_limiter_disable_vsync'),
];

export const settingsCategories: SettingsCategory[] = [
  {
    id: 'everyday',
    groups: [
      {
        id: 'everyday_display',
        fields: [
          ...everydayDisplayFields(),
          ...virtualDisplayCustomizationFields(),
          select('capture', captureOptions, {
            labelKey: 'ui.settings.fields.capture.label',
            descriptionKey: 'ui.settings.fields.capture.description',
            platform: 'windows',
            recommended: true,
          }),
        ],
      },
      {
        id: 'everyday_resolution',
        fields: [
          modeRemapping({
            labelKey: 'ui.settings.fields.dd_mode_remapping.label',
            descriptionKey: 'ui.settings.fields.dd_mode_remapping.description',
            simple: true,
          }),
        ],
      },
      {
        id: 'everyday_smoothness',
        visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
        fields: [
          select('frame_limiter_auto_virtual_framegen', frameGenerationOptions, {
            labelKey: 'ui.settings.fields.frame_limiter_auto_virtual_framegen.label',
            descriptionKey: 'ui.settings.fields.frame_limiter_auto_virtual_framegen.description',
            recommended: true,
            visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
          }),
        ],
      },
      {
        id: 'everyday_encoding',
        fields: [
          select('encoder', [option('', '_common.auto')], {
            labelKey: 'ui.settings.fields.encoder.label',
            descriptionKey: 'ui.settings.fields.encoder.description',
            recommended: true,
          }),
          select('nvenc_preset', nvencPresetOptions, { encoderFamily: 'nvidia' }),
          select('qsv_preset', qsvPresetOptions, { encoderFamily: 'intel' }),
          select('amd_quality', amdQualityOptions, { encoderFamily: 'amd' }),
          number('fec_percentage', {
            min: 1,
            max: 255,
            step: 1,
            labelKey: 'ui.settings.fields.fec_percentage.label',
            descriptionKey: 'ui.settings.fields.fec_percentage.description',
          }),
        ],
      },
      {
        id: 'everyday_recovery',
        fields: [
          displayRecovery(),
          boolean('dd_config_revert_on_disconnect', {
            labelKey: 'ui.settings.fields.dd_config_revert_on_disconnect.label',
            descriptionKey: 'ui.settings.fields.dd_config_revert_on_disconnect.description',
          }),
          duration(
            'dd_paused_virtual_display_timeout_secs',
            [
              option('0', 'ui.settings.options.paused_display_timeout.until_game_closes'),
              option('1800', 'ui.settings.options.paused_display_timeout.thirty_minutes'),
              option('3600', 'ui.settings.options.paused_display_timeout.one_hour'),
              option('7200', 'ui.settings.options.paused_display_timeout.two_hours'),
              option('14400', 'ui.settings.options.paused_display_timeout.four_hours'),
            ],
            {
              labelKey: 'ui.settings.fields.dd_paused_virtual_display_timeout_secs.label',
              descriptionKey:
                'ui.settings.fields.dd_paused_virtual_display_timeout_secs.description',
              warningKey: 'ui.settings.fields.dd_paused_virtual_display_timeout_secs.warning',
              recommended: true,
              visibleWhen: { key: 'dd_config_revert_on_disconnect', equals: false },
            },
          ),
        ],
      },
    ],
  },
  {
    id: 'display',
    groups: [
      {
        id: 'display_virtual',
        fields: [...everydayDisplayFields(), ...virtualDisplayCustomizationFields()],
      },
      {
        id: 'display_target',
        fields: [
          text('output_name', { monospace: true, stacked: true }),
          select('adapter_name', [], { source: 'gpu' }),
          select('dd_configuration_option', [
            option('verify_only', 'ui.settings.options.display_preparation.verify_only'),
            option('ensure_active', 'ui.settings.options.display_preparation.ensure_active'),
            option('ensure_primary', 'ui.settings.options.display_preparation.ensure_primary'),
            option('ensure_only_display', 'ui.settings.options.display_preparation.ensure_only'),
            option('disabled', '_common.disabled'),
          ]),
          select('dd_resolution_option', [
            option('auto', 'ui.settings.options.resolution.auto'),
            option('disabled', 'ui.settings.options.resolution.preserve'),
            option('manual', 'ui.settings.options.resolution.manual'),
          ]),
          text('dd_manual_resolution', {
            placeholderKey: 'ui.settings.placeholders.resolution',
            visibleWhen: { key: 'dd_resolution_option', equals: 'manual' },
          }),
          select('dd_refresh_rate_option', [
            option('auto', 'ui.settings.options.refresh.auto'),
            option('prefer_highest', 'ui.settings.options.refresh.highest'),
            option('disabled', 'ui.settings.options.refresh.preserve'),
            option('manual', 'ui.settings.options.refresh.manual'),
          ]),
          number('dd_manual_refresh_rate', {
            min: 1,
            max: 1000,
            step: 0.001,
            placeholderKey: 'ui.settings.placeholders.refresh_rate',
            visibleWhen: { key: 'dd_refresh_rate_option', equals: 'manual' },
          }),
          select('dd_hdr_option', [
            option('auto', 'ui.settings.options.hdr.auto'),
            option('disabled', 'ui.settings.options.hdr.preserve'),
          ]),
          select('dd_hdr_request_override', [
            option('auto', 'ui.settings.options.hdr_request.auto'),
            option('force_on', 'ui.settings.options.hdr_request.force_on'),
            option('force_off', 'ui.settings.options.hdr_request.force_off'),
          ]),
          modeRemapping({
            labelKey: 'ui.settings.fields.dd_mode_remapping.label',
            descriptionKey: 'ui.settings.fields.dd_mode_remapping.description',
          }),
        ],
      },
      {
        id: 'display_driver',
        fields: [
          boolean('dd_use_sunshine_virtual_display_driver', {
            labelKey: 'ui.settings.fields.dd_use_sunshine_virtual_display_driver.label',
            descriptionKey: 'ui.settings.fields.dd_use_sunshine_virtual_display_driver.description',
            recommended: true,
          }),
          boolean('dd_activate_virtual_display', {
            visibleWhen: { key: 'dd_use_sunshine_virtual_display_driver', equals: true },
          }),
          number('dd_virtual_display_permanent_count', {
            min: 0,
            max: 4,
            step: 1,
            visibleWhen: { key: 'dd_use_sunshine_virtual_display_driver', equals: true },
          }),
          select('dd_display_helper_engine', [
            option('auto', '_common.auto'),
            option('v2', 'ui.settings.options.display_engine.current'),
            option('legacy', 'ui.settings.options.display_engine.legacy'),
          ]),
          boolean('vulkan_hdr_layer'),
          boolean('dd_wa_dummy_plug_hdr10', {
            visibleWhen: { key: 'virtual_display_mode', equals: 'disabled' },
          }),
        ],
      },
      {
        id: 'display_recovery',
        fields: [
          boolean('dd_config_revert_on_disconnect'),
          boolean('dd_always_restore_from_golden'),
          number('dd_paused_virtual_display_timeout_secs', { min: 0, step: 1 }),
          text('dd_snapshot_restore_hotkey'),
          text('dd_snapshot_restore_hotkey_modifiers'),
        ],
      },
    ],
  },
  {
    id: 'pacing',
    groups: [
      {
        id: 'pacing_capture',
        fields: [select('capture', captureOptions), boolean('wgc_pacing_smoothing')],
      },
      { id: 'pacing_limiter', fields: everydayPacingFields() },
      {
        id: 'pacing_integrations',
        fields: [
          integrationPath('rtss_install_path', 'rtss', {
            labelKey: 'ui.settings.fields.rtss_install_path.label',
            descriptionKey: 'ui.settings.fields.rtss_install_path.description',
          }),
          select(
            'rtss_frame_limit_type',
            [
              option('async', 'ui.settings.options.rtss_type.async'),
              option('front edge sync', 'ui.settings.options.rtss_type.front_edge'),
              option('back edge sync', 'ui.settings.options.rtss_type.back_edge'),
              option('nvidia reflex', 'ui.settings.options.rtss_type.reflex'),
            ],
            { platform: 'windows' },
          ),
          integrationPath('lossless_scaling_path', 'lossless', {
            labelKey: 'ui.settings.fields.lossless_scaling_path.label',
            descriptionKey: 'ui.settings.fields.lossless_scaling_path.description',
          }),
          boolean('lossless_scaling_legacy_auto_detect', { platform: 'windows' }),
        ],
      },
    ],
  },
  {
    id: 'input',
    groups: [
      {
        id: 'input_devices',
        fields: [
          boolean('keyboard'),
          boolean('mouse'),
          boolean('controller'),
          boolean('motion_as_ds4'),
          boolean('touchpad_as_ds4'),
          boolean('ds4_back_as_touchpad_click'),
          boolean('always_send_scancodes'),
          boolean('high_resolution_scrolling'),
          boolean('native_pen_touch'),
        ],
      },
      {
        id: 'input_repeat',
        fields: [
          number('key_repeat_delay', { min: 0, step: 1 }),
          number('key_repeat_frequency', { min: 0.1, step: 0.1 }),
        ],
      },
    ],
  },
  {
    id: 'audio',
    groups: [
      {
        id: 'audio_routing',
        fields: [
          boolean('stream_audio'),
          text('audio_sink', { monospace: true, stacked: true }),
          text('virtual_sink', { monospace: true, stacked: true }),
          boolean('install_steam_audio_drivers'),
        ],
      },
    ],
  },
  {
    id: 'video',
    groups: [
      {
        id: 'video_encoder',
        fields: [
          select('encoder', [option('', '_common.auto')]),
          nvencPresetField(),
          boolean('wgc_pacing_smoothing'),
        ],
      },
      {
        id: 'video_codecs',
        fields: [
          select('hevc_mode', [
            option('0', '_common.auto'),
            option('1', '_common.disabled'),
            option('2', 'ui.settings.options.codec.eight_bit'),
            option('3', 'ui.settings.options.codec.hdr_ten_bit'),
          ]),
          select('av1_mode', [
            option('0', '_common.auto'),
            option('1', '_common.disabled'),
            option('2', 'ui.settings.options.codec.eight_bit'),
            option('3', 'ui.settings.options.codec.hdr_ten_bit'),
          ]),
        ],
      },
      {
        id: 'video_quality',
        fields: [
          number('max_bitrate', { min: 0, step: 1 }),
          number('minimum_fps_target', { min: 0, max: 1000, step: 0.1 }),
          number('qp', { min: 0, max: 51, step: 1 }),
          number('fec_percentage', { min: 0, max: 255, step: 1 }),
          number('video_max_batch_size_kb', { min: 1, step: 1 }),
        ],
      },
    ],
  },
  {
    id: 'network',
    groups: [
      {
        id: 'network_access',
        fields: [
          select('origin_web_ui_allowed', [
            option('pc', 'ui.settings.options.origin.pc'),
            option('lan', 'ui.settings.options.origin.lan'),
            option('wan', 'ui.settings.options.origin.wan'),
          ]),
          boolean('upnp', { restartRequired: true }),
          select(
            'address_family',
            [
              option('ipv4', 'ui.settings.options.address_family.ipv4'),
              option('both', 'ui.settings.options.address_family.both'),
            ],
            { restartRequired: true },
          ),
          number('port', { min: 1019, max: 65514, restartRequired: true }),
          text('bind_address', { monospace: true, stacked: true }),
          text('external_ip', { monospace: true, stacked: true }),
          number('ping_timeout', { min: 0, step: 1 }),
        ],
      },
      {
        id: 'network_security',
        fields: [
          select('lan_encryption_mode', [
            option('0', '_common.disabled'),
            option('1', 'ui.settings.options.encryption.optional'),
            option('2', 'ui.settings.options.encryption.required'),
          ]),
          select('wan_encryption_mode', [
            option('0', '_common.disabled'),
            option('1', 'ui.settings.options.encryption.optional'),
            option('2', 'ui.settings.options.encryption.required'),
          ]),
          text('csrf_allowed_origins', { stacked: true }),
        ],
      },
    ],
  },
  {
    id: 'host',
    groups: [
      {
        id: 'host_identity',
        fields: [
          text('sunshine_name', { placeholderKey: 'ui.settings.placeholders.host_name' }),
          boolean('system_tray'),
          boolean('notify_pre_releases'),
          select('min_log_level', [
            option('0', 'ui.settings.options.log_level.verbose'),
            option('1', 'ui.settings.options.log_level.debug'),
            option('2', 'ui.settings.options.log_level.info'),
            option('3', 'ui.settings.options.log_level.warning'),
            option('4', 'ui.settings.options.log_level.error'),
            option('5', 'ui.settings.options.log_level.fatal'),
            option('6', 'ui.settings.options.log_level.none'),
          ]),
        ],
      },
      {
        id: 'host_commands',
        fields: [
          {
            key: 'global_prep_cmd',
            kind: 'command-preparations',
            labelKey: 'config.global_prep_cmd',
            descriptionKey: 'config.global_prep_cmd_desc',
            stacked: true,
          },
        ],
      },
      {
        id: 'host_history',
        fields: [
          boolean('session_history_enabled'),
          number('session_history_ttl_days', {
            min: 0,
            step: 1,
            visibleWhen: { key: 'session_history_enabled', equals: true },
          }),
          number('session_history_db_size_limit_mb', {
            min: 0,
            step: 1,
            visibleWhen: { key: 'session_history_enabled', equals: true },
          }),
          boolean('realtime_stats_enabled'),
          number('realtime_stats_poll_interval_ms', {
            min: 250,
            max: 60000,
            step: 50,
            visibleWhen: { key: 'realtime_stats_enabled', equals: true },
          }),
        ],
      },
    ],
  },
  {
    id: 'files',
    groups: [
      {
        id: 'file_paths',
        fields: [
          text('file_apps', { monospace: true, stacked: true }),
          text('log_path', { monospace: true, stacked: true }),
          text('pkey', { monospace: true, restartRequired: true, stacked: true }),
          text('cert', { monospace: true, restartRequired: true, stacked: true }),
        ],
      },
    ],
  },
];

export const settingsDefaults: Record<string, unknown> = {
  virtual_display_mode: 'per_client',
  virtual_display_layout: 'exclusive',
  dd_virtual_display_scale: -1,
  frame_limiter_enable: false,
  frame_limiter_provider: 'auto',
  frame_limiter_fps_limit: 0,
  frame_limiter_auto_virtual_framegen: 'enabled',
  frame_limiter_disable_vsync: false,
  capture: '',
  stream_audio: true,
  controller: true,
  origin_web_ui_allowed: 'lan',
  upnp: false,
  output_name: '',
  adapter_name: '',
  adapter_pnp_id: '',
  dd_configuration_option: 'verify_only',
  dd_resolution_option: 'auto',
  dd_manual_resolution: '',
  dd_refresh_rate_option: 'auto',
  dd_manual_refresh_rate: '',
  dd_hdr_option: 'auto',
  dd_hdr_request_override: 'auto',
  dd_mode_remapping: {
    mixed: [],
    resolution_only: [],
    refresh_rate_only: [],
  },
  dd_use_sunshine_virtual_display_driver: true,
  dd_activate_virtual_display: false,
  dd_virtual_display_permanent_count: 0,
  dd_display_helper_engine: 'auto',
  vulkan_hdr_layer: true,
  dd_wa_dummy_plug_hdr10: false,
  dd_config_revert_on_disconnect: false,
  dd_always_restore_from_golden: true,
  dd_paused_virtual_display_timeout_secs: 7200,
  dd_snapshot_restore_hotkey: '',
  dd_snapshot_restore_hotkey_modifiers: 'ctrl+alt+shift',
  keyboard: true,
  mouse: true,
  motion_as_ds4: true,
  touchpad_as_ds4: true,
  ds4_back_as_touchpad_click: true,
  always_send_scancodes: true,
  high_resolution_scrolling: true,
  native_pen_touch: true,
  key_repeat_delay: 500,
  key_repeat_frequency: 24.9,
  install_steam_audio_drivers: true,
  audio_sink: '',
  virtual_sink: '',
  encoder: '',
  nvenc_preset: 1,
  qsv_preset: 'medium',
  amd_quality: 'balanced',
  wgc_pacing_smoothing: true,
  hevc_mode: 0,
  av1_mode: 0,
  max_bitrate: 0,
  minimum_fps_target: 20,
  qp: 28,
  fec_percentage: 20,
  video_max_batch_size_kb: 64,
  rtss_install_path: '',
  rtss_frame_limit_type: 'async',
  lossless_scaling_path: '',
  lossless_scaling_legacy_auto_detect: false,
  address_family: 'ipv4',
  port: 47989,
  bind_address: '',
  external_ip: '',
  ping_timeout: 10000,
  lan_encryption_mode: 0,
  wan_encryption_mode: 1,
  csrf_allowed_origins: '',
  system_tray: true,
  notify_pre_releases: false,
  min_log_level: 2,
  global_prep_cmd: [],
  session_history_enabled: true,
  session_history_ttl_days: 0,
  session_history_db_size_limit_mb: 0,
  realtime_stats_enabled: true,
  realtime_stats_poll_interval_ms: 2000,
};

export const knownSettingsKeys = new Set(
  settingsCategories.flatMap((category) =>
    category.groups.flatMap((group) => group.fields.map((field) => field.key)),
  ),
);
knownSettingsKeys.add('adapter_pnp_id');

export const restartRequiredKeys = new Set(['address_family', 'cert', 'pkey', 'port', 'upnp']);
