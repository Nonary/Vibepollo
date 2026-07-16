import {
  getConfigSelectOptions,
  type ConfigSelectOption,
  type ConfigSelectOptionsContext,
} from './configSelectOptions';

export type ConfigFieldKind =
  | 'checkbox'
  | 'switch'
  | 'select'
  | 'number'
  | 'slider'
  | 'input'
  | 'textarea';

export type ConfigFieldDefinition = {
  kind: ConfigFieldKind;
  options?: ConfigSelectOption[];
  durationUnit?: 'seconds';
  placeholder?: string;
  clearable?: boolean;
  filterable?: boolean;
  monospace?: boolean;
  autosize?: boolean | { minRows: number; maxRows: number };
  inputmode?: string;
  min?: number;
  max?: number;
  step?: number;
  precision?: number;
  localePrefix?: string;
  inverseValues?: boolean;
};

export type ConfigFieldSchemaContext = ConfigSelectOptionsContext & {
  currentValue?: unknown;
  defaultValue?: unknown;
  kind?: ConfigFieldKind;
  options?: ConfigSelectOption[];
};

const SWITCH_KEYS = new Set<string>([
  'frame_limiter_enable',
  'frame_limiter_disable_vsync',
  'rtx_hdr',
  'rtx_hdr_force_sdr',
]);

const NUMBER_FIELD_OVERRIDES: Record<string, Partial<ConfigFieldDefinition>> = {
  fec_percentage: { placeholder: '20' },
  qp: { placeholder: '28' },
  min_threads: { placeholder: '2', min: 1 },
  back_button_timeout: { placeholder: '-1' },
  key_repeat_delay: { placeholder: '500' },
  key_repeat_frequency: { placeholder: '24.9', step: 0.1 },
  session_token_ttl_seconds: { min: 60, step: 60, placeholder: '86400' },
  remember_me_refresh_token_ttl_seconds: { min: 3600, step: 3600, placeholder: '604800' },
  session_history_ttl_days: { min: 0, step: 1, placeholder: '0' },
  session_history_db_size_limit_mb: { min: 0, step: 1, placeholder: '0' },
  realtime_stats_poll_interval_ms: { min: 250, max: 60000, step: 250, placeholder: '2000' },
  realtime_stats_history_retention_seconds: { min: 30, max: 3600, step: 30, placeholder: '300' },
  realtime_stats_max_history_points: {
    min: 30,
    max: 2000,
    step: 10,
    precision: 0,
    placeholder: '300',
  },
  update_check_interval: { min: 0, step: 60, placeholder: '86400' },
  port: { min: 1029, max: 65514, placeholder: '47989' },
  ping_timeout: { min: 0, step: 100, placeholder: '10000' },
  max_bitrate: { min: 0, placeholder: '0' },
  minimum_fps_target: { min: 0, max: 1000, placeholder: '0' },
  rtx_hdr_contrast: { min: -100, max: 100, step: 1, placeholder: '0' },
  rtx_hdr_saturation: { min: -100, max: 100, step: 1, placeholder: '0' },
  rtx_hdr_sdr_brightness: { min: 0, max: 100, step: 1, placeholder: '0' },
  rtx_hdr_middle_gray: { min: 10, max: 100, step: 1, placeholder: '50' },
  rtx_hdr_peak_brightness: { min: 400, max: 2000, step: 1, placeholder: '1000' },
  dd_virtual_display_permanent_count: { min: 0, max: 4, step: 1, precision: 0, placeholder: '0' },
  nvenc_vbv_increase: { min: 0, max: 400, placeholder: '0' },
  frame_limiter_fps_limit: { min: 0, max: 1000, step: 1, precision: 0, placeholder: '0' },
};

const SLIDER_KEYS = new Set<string>([
  'rtx_hdr_contrast',
  'rtx_hdr_saturation',
  'rtx_hdr_sdr_brightness',
  'rtx_hdr_middle_gray',
  'rtx_hdr_peak_brightness',
]);

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
}

function inferDurationUnit(key: string): ConfigFieldDefinition['durationUnit'] {
  if (key === 'update_check_interval') return 'seconds';
  if (key.endsWith('_seconds') || key.endsWith('_secs')) return 'seconds';
  return undefined;
}

function getDurationUnitDefinition(key: string): Pick<ConfigFieldDefinition, 'durationUnit'> {
  const durationUnit = inferDurationUnit(key);
  return durationUnit ? { durationUnit } : {};
}

function kindSampleValue(ctx: ConfigFieldSchemaContext): unknown {
  // Anchor known config fields to their default type so the rendered control
  // does not change while the user edits the value.
  if (ctx.defaultValue !== undefined) return ctx.defaultValue;
  return ctx.currentValue;
}

function isBooleanLike(value: unknown): boolean {
  if (value === true || value === false) return true;
  if (value === 1 || value === 0) return true;
  if (typeof value !== 'string') return false;

  const normalized = value.toLowerCase().trim();
  return [
    'true',
    'false',
    '1',
    '0',
    'enabled',
    'disabled',
    'enable',
    'disable',
    'yes',
    'no',
    'on',
    'off',
  ].includes(normalized);
}

export function prettifyConfigKey(key: string): string {
  return key
    .split('_')
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join(' ');
}

export function getConfigFieldDefinition(
  key: string,
  ctx: ConfigFieldSchemaContext,
): ConfigFieldDefinition {
  if (ctx.kind) {
    const overrideOptions =
      ctx.options ??
      (ctx.kind === 'select'
        ? getConfigSelectOptions(key, {
            t: ctx.t,
            platform: ctx.platform,
            // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment -- config metadata is intentionally schema-driven
            metadata: ctx.metadata,
            currentValue: ctx.currentValue,
          })
        : undefined);

    return {
      kind: ctx.kind,
      ...(ctx.kind === 'select' && overrideOptions
        ? { options: overrideOptions, filterable: true }
        : ctx.kind === 'select'
          ? { filterable: true }
          : {}),
      ...(ctx.kind === 'number' || ctx.kind === 'slider'
        ? {
            ...(NUMBER_FIELD_OVERRIDES[key] ?? {}),
            ...getDurationUnitDefinition(key),
          }
        : {}),
      localePrefix: 'config',
    };
  }

  const selectOptions =
    ctx.options ??
    getConfigSelectOptions(key, {
      t: ctx.t,
      platform: ctx.platform,
      // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment -- config metadata is intentionally schema-driven
      metadata: ctx.metadata,
      currentValue: ctx.currentValue,
    });

  if (selectOptions.length > 0) {
    return {
      kind: 'select',
      options: selectOptions,
      filterable: selectOptions.length >= 8,
    };
  }

  if (SWITCH_KEYS.has(key)) {
    return {
      kind: 'switch',
    };
  }

  const sampleValue = kindSampleValue(ctx);

  if (
    Object.prototype.hasOwnProperty.call(NUMBER_FIELD_OVERRIDES, key) ||
    isFiniteNumber(sampleValue)
  ) {
    return {
      kind: SLIDER_KEYS.has(key) ? 'slider' : 'number',
      ...(NUMBER_FIELD_OVERRIDES[key] ?? {}),
      ...getDurationUnitDefinition(key),
    };
  }

  if (isBooleanLike(sampleValue)) {
    return {
      kind: 'checkbox',
      localePrefix: 'config',
    };
  }

  return {
    kind: 'input',
  };
}
