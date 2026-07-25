import { getConfigFieldDefinition } from '@web/configs/configFieldSchema';

const baseContext = {
  t: (key: string) => key,
  platform: 'windows',
  metadata: {},
};

describe('configFieldSchema', () => {
  test.each([0, 1, '0', '1'])(
    'keeps back_button_timeout as a number field for %p',
    (currentValue) => {
      expect(
        getConfigFieldDefinition('back_button_timeout', {
          ...baseContext,
          defaultValue: -1,
          currentValue,
        }).kind,
      ).toBe('number');
    },
  );

  test('anchors known fields to the default type instead of the live edited value', () => {
    expect(
      getConfigFieldDefinition('system_tray', {
        ...baseContext,
        defaultValue: true,
        currentValue: '0',
      }).kind,
    ).toBe('checkbox');

    expect(
      getConfigFieldDefinition('remember_me_refresh_token_ttl_seconds', {
        ...baseContext,
        defaultValue: 604800,
        currentValue: 'enabled',
      }).kind,
    ).toBe('number');
  });

  test('falls back to the current value when no default is available', () => {
    expect(
      getConfigFieldDefinition('unknown_number_key', {
        ...baseContext,
        currentValue: 1,
      }).kind,
    ).toBe('number');

    expect(
      getConfigFieldDefinition('unknown_bool_key', {
        ...baseContext,
        currentValue: 'enabled',
      }).kind,
    ).toBe('checkbox');
  });

  test.each([
    'amd_quality',
    'amd_vbaq',
    'amd_av1_screen_content',
    'amd_av1_latency_mode',
  ])('renders %s as an auto-capable select', (settingKey) => {
    // currentValue stays undefined so ensureIncludesCurrentValue cannot append
    // 'auto' as a fallback — the option must come from the select definition.
    const field = getConfigFieldDefinition(settingKey, {
      ...baseContext,
      defaultValue: 'auto',
      currentValue: undefined,
    });

    expect(field.kind).toBe('select');
    expect(field.options?.map(({ value }) => value)).toContain('auto');
  });

  test('amd_rc has no auto option — the backend has no auto branch for it', () => {
    const field = getConfigFieldDefinition('amd_rc', {
      ...baseContext,
      defaultValue: 'vbr_latency',
      currentValue: undefined,
    });

    expect(field.kind).toBe('select');
    expect(field.options?.map(({ value }) => value)).not.toContain('auto');
  });

  test('exposes every native AMF quality preset', () => {
    const field = getConfigFieldDefinition('amd_quality', {
      ...baseContext,
      defaultValue: 'balanced',
      currentValue: 'balanced',
    });

    expect(field.options?.map(({ value }) => value)).toEqual([
      'auto',
      'speed',
      'balanced',
      'quality',
    ]);
  });
});
