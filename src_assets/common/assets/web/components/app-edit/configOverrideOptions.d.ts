import type {
  ConfigSelectOption,
  ConfigSelectOptionsContext,
} from '../../configs/configSelectOptions';

export type OverrideSelectOption = ConfigSelectOption;
export type OverrideSelectOptionsContext = ConfigSelectOptionsContext;

export declare function isBooleanOverrideKey(key: string): boolean;
export declare function getOverrideSelectOptions(
  key: string,
  ctx: OverrideSelectOptionsContext,
): OverrideSelectOption[];
export declare function buildOverrideOptionsText(options: OverrideSelectOption[]): string;
