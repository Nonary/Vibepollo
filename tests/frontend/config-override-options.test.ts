import { getConfigSelectOptions } from '@web/configs/configSelectOptions';
import {
  getOverrideSelectOptions,
  isBooleanOverrideKey,
} from '@web/components/app-edit/configOverrideOptions';

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

  test.each(['enabled', 0])(
    'leaves known boolean override value %p to the checkbox renderer',
    (currentValue) => {
      const overrideOptions = getOverrideSelectOptions('stream_audio', {
        ...baseContext,
        currentValue,
      });

      expect(overrideOptions).toEqual([]);
    },
  );

  test('classifies VideoToolbox realtime overrides as boolean', () => {
    expect(isBooleanOverrideKey('vt_realtime')).toBe(true);
    expect(
      getOverrideSelectOptions('vt_realtime', {
        ...baseContext,
        currentValue: 'enabled',
      }),
    ).toEqual([]);
  });

  test('does not classify boolean-looking free-form strings as booleans', () => {
    expect(isBooleanOverrideKey('dd_snapshot_restore_hotkey_modifiers')).toBe(false);

    const overrideOptions = getOverrideSelectOptions('dd_snapshot_restore_hotkey_modifiers', {
      ...baseContext,
      currentValue: 'disabled',
    });

    // The key declares no choices, so it must fall through to the free-text
    // editor instead of a single-entry dropdown that cannot be typed into.
    expect(overrideOptions).toEqual([]);
  });

  test('does not synthesize a dropdown for keys without declared choices', () => {
    expect(getOverrideSelectOptions('max_bitrate', { ...baseContext, currentValue: 0 })).toEqual(
      [],
    );
    expect(getOverrideSelectOptions('audio_sink', { ...baseContext, currentValue: '' })).toEqual(
      [],
    );
  });

  test('keeps declared enabled and disabled select choices', () => {
    const overrideOptions = getOverrideSelectOptions('amd_vbaq', {
      ...baseContext,
      currentValue: 'enabled',
    });

    expect(overrideOptions.map((option) => option.value)).toEqual(['auto', 'enabled', 'disabled']);
  });
});
