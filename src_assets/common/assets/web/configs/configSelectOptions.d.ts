export type ConfigSelectOption = {
  label: string;
  value: string | number;
  disabled?: boolean;
};

export type ConfigSelectOptionsContext = {
  t: (key: string) => string;
  platform: string;
  metadata?: any;
  currentValue?: unknown;
};

export declare function getConfigSelectOptions(
  key: string,
  ctx: ConfigSelectOptionsContext,
): ConfigSelectOption[];
export declare function buildConfigOptionsText(options: ConfigSelectOption[]): string;
