import {
  advancedEncoderGroups,
  extendedDefaults,
  extendedField,
  playnitePolicyFields,
} from './extendedSettings.ts';
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
  | 'command-preparations'
  | 'server-commands';

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
  platform?: 'windows' | 'linux' | 'macos' | Array<'windows' | 'linux' | 'macos'>;
  visibleWhen?: SettingsVisibility;
  source?: 'gpu';
  encoderFamily?: 'nvidia' | 'intel' | 'amd' | 'vaapi' | 'vulkan' | 'videotoolbox' | 'software';
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
  'audio_sink_capture_only',
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
  platform?: SettingsField['platform'];
  link?: string;
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

export function captureOptionsForPlatform(platform: string): SettingsOption[] {
  const normalized = platform.toLocaleLowerCase();
  const supportedValues = normalized.includes('windows')
    ? new Set(['', 'wgc', 'wgcc', 'ddx'])
    : normalized.includes('linux')
      ? new Set(['', 'kms', 'kwin', 'portal', 'wlr', 'x11', 'nvfbc'])
      : new Set(['']);
  return captureOptions.filter((candidate) => supportedValues.has(candidate.value));
}

const gamepadOptions = [
  option('auto', '_common.auto'),
  option('x360', 'config.gamepad_x360'),
  option('xone', 'config.gamepad_xone'),
  option('ds4', 'config.gamepad_ds4'),
  option('ds5', 'config.gamepad_ds5'),
  option('switch', 'config.gamepad_switch'),
  option('vhf', 'config.gamepad_vhf'),
  option('vhf_xbox', 'config.gamepad_vhf_xbox'),
  option('vhf_xbox_one', 'config.gamepad_vhf_xbox_one'),
  option('vhf_ds4', 'config.gamepad_vhf_ds4'),
  option('vhf_ds5', 'config.gamepad_vhf_ds5'),
  option('vhf_switch', 'config.gamepad_vhf_switch'),
];

export function gamepadOptionsForPlatform(platform: string): SettingsOption[] {
  const normalized = platform.toLocaleLowerCase();
  const supportedValues = normalized.includes('windows')
    ? new Set([
        'auto',
        'x360',
        'ds4',
        'vhf',
        'vhf_xbox',
        'vhf_xbox_one',
        'vhf_ds4',
        'vhf_ds5',
        'vhf_switch',
      ])
    : normalized.includes('linux')
      ? new Set(['auto', 'xone', 'ds4', 'ds5', 'switch'])
      : new Set(['auto']);
  return gamepadOptions.filter((candidate) => supportedValues.has(candidate.value));
}

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

export function frameGenerationOptionsForPlatform(platform: string): SettingsOption[] {
  if (!platform.toLocaleLowerCase().includes('linux')) return frameGenerationOptions;
  return [
    option('enabled', 'ui.settings.options.frame_generation.automatic_linux'),
    option('legacy', 'ui.settings.options.frame_generation.compatibility_linux'),
    option('disabled', 'ui.settings.options.frame_generation.off_linux'),
  ];
}

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

const remoteMonitorFields = (): SettingsField[] => [
  boolean('remote_monitor_confirm_app_replacement', {
    labelKey: 'ui.settings.fields.remote_monitor_confirm_app_replacement.label',
    descriptionKey: 'ui.settings.fields.remote_monitor_confirm_app_replacement.description',
  }),
  boolean('remote_monitor_mute_audio', {
    labelKey: 'ui.settings.fields.remote_monitor_mute_audio.label',
    descriptionKey: 'ui.settings.fields.remote_monitor_mute_audio.description',
    platform: 'windows',
  }),
  boolean('remote_monitor_disconnect_on_stream_end', {
    labelKey: 'ui.settings.fields.remote_monitor_disconnect_on_stream_end.label',
    descriptionKey: 'ui.settings.fields.remote_monitor_disconnect_on_stream_end.description',
    platform: 'windows',
  }),
  boolean('remote_monitor_disconnect_on_client_disconnect', {
    labelKey: 'ui.settings.fields.remote_monitor_disconnect_on_client_disconnect.label',
    descriptionKey: 'ui.settings.fields.remote_monitor_disconnect_on_client_disconnect.description',
    platform: 'windows',
  }),
  boolean('remote_monitor_terminate_on_first_request', {
    labelKey: 'ui.settings.fields.remote_monitor_terminate_on_first_request.label',
    descriptionKey: 'ui.settings.fields.remote_monitor_terminate_on_first_request.description',
    platform: 'windows',
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
        fields: [everydayDisplayFields()[0], virtualDisplayCustomizationFields()[0]],
      },
      {
        id: 'everyday_appearance',
        fields: [
          ...everydayDisplayFields().filter(
            (field) =>
              ![
                'virtual_display_mode',
                'dd_configuration_option',
                'dd_hdr_request_override',
              ].includes(field.key),
          ),
          virtualDisplayCustomizationFields()[1],
        ],
      },
      {
        id: 'everyday_remote_monitor',
        fields: remoteMonitorFields(),
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
        platform: 'windows',
        visibleWhen: { key: 'virtual_display_mode', notEquals: 'disabled' },
        fields: [
          select('frame_limiter_auto_virtual_framegen', frameGenerationOptions, {
            platform: 'windows',
          }),
        ],
      },
      {
        id: 'everyday_audio',
        link: '/settings?category=audio',
        fields: [
          select('keep_sink_default', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('auto_capture_sink', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          boolean('stream_audio'),
        ],
      },
      {
        id: 'everyday_input',
        link: '/settings?category=input',
        fields: [
          select('enable_input_only_mode', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('forward_rumble', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          boolean('controller'),
          select('gamepad', gamepadOptions, {
            platform: ['windows', 'linux'],
            visibleWhen: { key: 'controller', equals: true },
          }),
          boolean('keyboard'),
          boolean('mouse'),
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
        id: 'display_remote_monitor',
        fields: remoteMonitorFields(),
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
          displayRecovery(),
          {
            key: 'dd_snapshot_exclude_devices',
            kind: 'textarea',
            platform: 'windows',
            stacked: true,
          },
          boolean('dd_config_revert_on_disconnect', { platform: ['windows', 'linux'] }),
          number('dd_config_revert_delay', {
            min: 0,
            step: 100,
            platform: ['windows', 'linux'],
            labelKey: 'ui.settings.fields.dd_config_revert_delay.label',
            descriptionKey: 'ui.settings.fields.dd_config_revert_delay.description',
          }),
          boolean('dd_always_restore_from_golden', { platform: 'windows' }),
          number('dd_paused_virtual_display_timeout_secs', {
            min: 0,
            step: 1,
            platform: ['windows', 'linux'],
          }),
          text('dd_snapshot_restore_hotkey_modifiers', { platform: 'windows' }),
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
      {
        id: 'pacing_limiter',
        fields: [
          ...everydayPacingFields(),
          boolean('rtss_allow_virtual_display_override', {
            platform: 'windows',
            warningKey: 'ui.settings.fields.rtss_allow_virtual_display_override.warning',
          }),
        ],
      },
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
            {
              labelKey: 'ui.settings.fields.rtss_frame_limit_type.label',
              descriptionKey: 'ui.settings.fields.rtss_frame_limit_type.description',
              platform: 'windows',
            },
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
          select(
            'gamepad',
            [
              option('auto', '_common.auto'),
              option('x360', 'config.gamepad_x360'),
              option('ds4', 'config.gamepad_ds4'),
              option('vhf', 'config.gamepad_vhf'),
              option('vhf_xbox', 'config.gamepad_vhf_xbox'),
              option('vhf_xbox_one', 'config.gamepad_vhf_xbox_one'),
              option('vhf_ds4', 'config.gamepad_vhf_ds4'),
              option('vhf_ds5', 'config.gamepad_vhf_ds5'),
              option('vhf_switch', 'config.gamepad_vhf_switch'),
            ],
            { platform: 'windows' },
          ),
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
          extendedField('back_button_timeout'),
          extendedField('key_rightalt_to_key_win'),
          extendedField('ds5_inputtino_randomize_mac', { platform: 'linux' }),
          { key: 'keybindings', kind: 'textarea', stacked: true },
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
          boolean('audio_sink_capture_only', { platform: 'windows' }),
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
        id: 'video_rtx_hdr',
        platform: 'windows',
        collapsed: true,
        fields: Object.keys(extendedDefaults)
          .filter((key) => key.startsWith('rtx_hdr'))
          .map((key) => extendedField(key, { platform: 'windows' })),
      },
      {
        id: 'video_encoder',
        fields: [
          select('encoder', [option('', '_common.auto')]),
          boolean('wgc_pacing_smoothing', { platform: 'windows' }),
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
          extendedField('min_threads', { min: 1, step: 1 }),
          number('qp', { min: 0, max: 51, step: 1 }),
          number('fec_percentage', { min: 0, max: 255, step: 1 }),
          number('video_max_batch_size_kb', { min: 1, step: 1 }),
        ],
      },
      ...advancedEncoderGroups,
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
          select('enable_pairing', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('enable_discovery', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
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
          extendedField('session_token_ttl_seconds'),
          extendedField('remember_me_refresh_token_ttl_seconds'),
          text('csrf_allowed_origins', { stacked: true }),
        ],
      },
    ],
  },
  {
    id: 'host',
    groups: [
      {
        id: 'everyday_automation',
        collapsed: true,
        fields: [
          select('limit_framerate', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('envvar_compatibility_mode', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('legacy_ordering', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          select('ignore_encoder_probe_failure', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          { key: 'global_prep_cmd', kind: 'command-preparations', stacked: true },
          {
            key: 'global_state_cmd',
            kind: 'command-preparations',
            labelKey: 'config.global_state_cmd',
            descriptionKey: 'config.global_state_cmd_desc',
            stacked: true,
          },
          {
            key: 'server_cmd',
            kind: 'server-commands',
            labelKey: 'config.server_cmd',
            descriptionKey: 'config.server_cmd_desc',
            stacked: true,
          },
        ],
      },
      {
        id: 'host_identity',
        fields: [
          select('hide_tray_controls', [
            option('enabled', '_common.enabled'),
            option('disabled', '_common.disabled'),
          ]),
          extendedField('locale'),
          extendedField('update_check_interval'),
          text('fallback_mode'),
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
        id: 'host_history',
        fields: [
          ...Object.keys(extendedDefaults)
            .filter(
              (key) =>
                key.startsWith('realtime_stats_') &&
                !['realtime_stats_enabled', 'realtime_stats_poll_interval_ms'].includes(key),
            )
            .map((key) =>
              extendedField(key, { visibleWhen: { key: 'realtime_stats_enabled', equals: true } }),
            ),
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
          extendedField('credentials_file', { monospace: true, stacked: true }),
          text('vibeshine_file_state', { monospace: true, stacked: true }),
          extendedField('file_state', { monospace: true, stacked: true }),
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
  enable_pairing: 'enabled',
  enable_discovery: 'enabled',
  hide_tray_controls: 'disabled',
  keep_sink_default: 'enabled',
  auto_capture_sink: 'enabled',
  enable_input_only_mode: 'disabled',
  forward_rumble: 'enabled',
  limit_framerate: 'enabled',
  envvar_compatibility_mode: 'disabled',
  legacy_ordering: 'disabled',
  ignore_encoder_probe_failure: 'disabled',
  fallback_mode: '1920x1080x60',

  ...extendedDefaults,
  gamepad: 'auto',
  virtual_display_mode: 'per_client',
  virtual_display_layout: 'exclusive',
  remote_monitor_mute_audio: false,
  remote_monitor_disconnect_on_stream_end: false,
  remote_monitor_disconnect_on_client_disconnect: false,
  remote_monitor_terminate_on_first_request: false,
  remote_monitor_confirm_app_replacement: true,
  dd_virtual_display_scale: -1,
  frame_limiter_enable: false,
  frame_limiter_provider: 'auto',
  frame_limiter_fps_limit: 0,
  frame_limiter_auto_virtual_framegen: 'enabled',
  frame_limiter_disable_vsync: false,
  rtss_allow_virtual_display_override: false,
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
  audio_sink_capture_only: false,
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
  global_state_cmd: [],
  server_cmd: [],
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

export function matchesPlatform(
  field: { platform?: SettingsField['platform'] },
  platform: string,
): boolean {
  if (!field.platform) return true;
  const normalized = platform.toLowerCase().replace('darwin', 'macos');
  const supported = Array.isArray(field.platform) ? field.platform : [field.platform];
  return supported.some((value) => normalized.includes(value === 'macos' ? 'mac' : value));
}

export function encoderFamilyFor(encoder: string): SettingsField['encoderFamily'] | undefined {
  if (encoder.startsWith('nvenc')) return 'nvidia';
  if (encoder.startsWith('amdvce')) return 'amd';
  if (encoder === 'quicksync') return 'intel';
  if (['vaapi', 'vulkan', 'videotoolbox', 'software'].includes(encoder))
    return encoder as SettingsField['encoderFamily'];
  return undefined;
}

export function encoderOptionsForPlatform(platform: string): SettingsOption[] {
  const p = platform.toLowerCase();
  const encoders = p.includes('windows')
    ? ['nvenc', 'quicksync', 'amdvce_ffmpeg', 'amdvce_experimental', 'mediafoundation', 'software']
    : p.includes('linux')
      ? ['nvenc', 'nvenc_legacy', 'vulkan', 'vaapi', 'software']
      : p.includes('mac')
        ? ['videotoolbox', 'software']
        : [];
  return [
    option('', 'ui.settings.options.encoder.auto'),
    ...encoders.map((value) => option(value, `ui.settings.options.encoder.${value}`)),
  ];
}

export function optionsForPlatform(field: SettingsField, platform: string): SettingsOption[] {
  if (field.key === 'encoder') return encoderOptionsForPlatform(platform);
  if (field.key === 'capture') return captureOptionsForPlatform(platform);
  if (field.key === 'gamepad') return gamepadOptionsForPlatform(platform);
  if (field.key === 'frame_limiter_auto_virtual_framegen')
    return frameGenerationOptionsForPlatform(platform);
  if (field.key === 'virtual_display_mode' && platform.toLowerCase().includes('linux'))
    return [
      option('per_client', 'ui.settings.linux.per_client'),
      option('shared', 'ui.settings.linux.shared'),
      option('disabled', 'ui.settings.options.virtual_display_mode.physical'),
    ];
  if (field.key === 'frame_limiter_provider' && platform.toLowerCase().includes('linux'))
    return [
      option('auto', 'ui.settings.options.frame_limiter_provider.autoLinux'),
      ...['mangohud', 'proton', 'mangohud-proton', 'none'].map((value) =>
        option(
          value,
          `ui.settings.options.frame_limiter_provider.${value === 'mangohud-proton' ? 'mangohudProton' : value}`,
        ),
      ),
    ];
  return field.options ?? [];
}

// Canonical definitions prefer the dedicated category over Everyday shortcuts.
export const settingsFields = new Map(
  settingsCategories.flatMap((category) =>
    category.groups.flatMap((group) => group.fields.map((field) => [field.key, field] as const)),
  ),
);
export function fieldForPlatform(field: SettingsField, platform: string): SettingsField {
  return field.source === 'gpu' && !platform.toLowerCase().includes('windows')
    ? { ...field, kind: 'text', source: undefined, monospace: true }
    : field;
}

export const settingsDestinations: Array<{
  labelKey: string;
  to: string;
  keys: string[];
  platform?: SettingsField['platform'];
}> = [
  {
    labelKey: 'ui.integrations.playnite.policies_title',
    to: '/integrations#playnite-policies',
    platform: 'windows',
    keys: playnitePolicyFields
      .map((field) => field.key)
      .concat([
        'playnite',
        'playnite_sync_categories',
        'playnite_exclude_games',
        'playnite_exclude_categories',
        'playnite_sync_plugins',
        'playnite_exclude_plugins',
      ]),
  },
  {
    labelKey: 'ui.integrations.steam.name',
    to: '/integrations#integration-steam',
    keys: ['steam', 'steam_auto_sync', 'steam_exclude_games', 'steam_include_tools'],
  },
  {
    labelKey: 'ui.integrations.lutris.name',
    to: '/integrations#integration-lutris',
    platform: 'linux',
    keys: ['lutris', 'lutris_auto_sync', 'lutris_include_steam'],
  },
  {
    labelKey: 'ui.integrations.mangohud.name',
    to: '/integrations#integration-mangohud',
    platform: 'linux',
    keys: ['mangohud_preset', 'mangohud_always_show_graph', 'overlay'],
  },
];
