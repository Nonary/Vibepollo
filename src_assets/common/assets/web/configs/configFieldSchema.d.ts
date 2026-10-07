import type { ConfigSelectOption, ConfigSelectOptionsContext } from './configSelectOptions';

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

export declare function prettifyConfigKey(key: string): string;
export declare function getConfigFieldDefinition(
  key: string,
  ctx: ConfigFieldSchemaContext,
): ConfigFieldDefinition;
