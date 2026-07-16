import { getConfigSelectOptions } from '@web/configs/configSelectOptions';
import { getOverrideSelectOptions } from '@web/components/app-edit/configOverrideOptions';

const baseContext = {
  t: (key: string) => key,
  platform: 'windows',
};

describe('configOverrideOptions', () => {
  test('stringifies numeric-backed select values for override payloads', () => {
    const configOptions = getConfigSelectOptions('min_log_level', {
      ...baseContext,
      currentValue: 1,
    });
    const overrideOptions = getOverrideSelectOptions('min_log_level', {
      ...baseContext,
      currentValue: 1,
    });

    expect(configOptions.some((option) => typeof option.value === 'number')).toBe(true);
    expect(overrideOptions.map((option) => option.value)).toEqual([
      '0',
      '1',
      '2',
      '3',
      '4',
      '5',
      '6',
    ]);
  });

  test('deduplicates saved string values that match numeric schema options', () => {
    const overrideOptions = getOverrideSelectOptions('min_log_level', {
      ...baseContext,
      currentValue: '1',
    });

    expect(overrideOptions.filter((option) => option.value === '1')).toHaveLength(1);
  });
});
