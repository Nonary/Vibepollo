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

  if (isOverrideSelectValue(ctx.currentValue)) {
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
