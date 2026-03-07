import {
  getConfigSelectOptions,
  type ConfigSelectOption,
  type ConfigSelectOptionsContext,
} from './configSelectOptions';

export type ConfigFieldKind = 'checkbox' | 'switch' | 'select' | 'number' | 'input' | 'textarea';

export type ConfigFieldDefinition = {
  kind: ConfigFieldKind;
  options?: ConfigSelectOption[];
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

const SWITCH_KEYS = new Set<string>(['frame_limiter_enable', 'frame_limiter_disable_vsync']);

const NUMBER_FIELD_OVERRIDES: Record<string, Partial<ConfigFieldDefinition>> = {
  fec_percentage: { placeholder: '20' },
  qp: { placeholder: '28' },
  min_threads: { placeholder: '2', min: 1 },
  back_button_timeout: { placeholder: '-1' },
  key_repeat_delay: { placeholder: '500' },
  key_repeat_frequency: { placeholder: '24.9', step: 0.1 },
  session_token_ttl_seconds: { min: 60, step: 60, placeholder: '86400' },
  remember_me_refresh_token_ttl_seconds: { min: 3600, step: 3600, placeholder: '604800' },
  update_check_interval: { min: 0, step: 60, placeholder: '86400' },
  port: { min: 1029, max: 65514, placeholder: '47989' },
  ping_timeout: { min: 0, step: 100, placeholder: '10000' },
  max_bitrate: { min: 0, placeholder: '0' },
  minimum_fps_target: { min: 0, max: 1000, placeholder: '0' },
  nvenc_vbv_increase: { min: 0, max: 400, placeholder: '0' },
  frame_limiter_fps_limit: { min: 0, max: 1000, step: 1, precision: 0, placeholder: '0' },
};

function isFiniteNumber(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value);
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
      ...(ctx.kind === 'number' ? (NUMBER_FIELD_OVERRIDES[key] ?? {}) : {}),
      localePrefix: 'config',
    };
  }

  const selectOptions =
    ctx.options ??
    getConfigSelectOptions(key, {
      t: ctx.t,
      platform: ctx.platform,
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

  if (isBooleanLike(ctx.currentValue) || isBooleanLike(ctx.defaultValue)) {
    return {
      kind: 'checkbox',
      localePrefix: 'config',
    };
  }

  if (
    Object.prototype.hasOwnProperty.call(NUMBER_FIELD_OVERRIDES, key) ||
    isFiniteNumber(ctx.currentValue) ||
    isFiniteNumber(ctx.defaultValue)
  ) {
    return {
      kind: 'number',
      ...(NUMBER_FIELD_OVERRIDES[key] ?? {}),
    };
  }

  return {
    kind: 'input',
  };
}
