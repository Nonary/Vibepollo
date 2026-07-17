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

  test('keeps the native AMF queue override on Auto by default', () => {
    const field = getConfigFieldDefinition('amd_input_queue_size', {
      ...baseContext,
      defaultValue: 0,
      currentValue: 0,
    });

    expect(field.kind).toBe('number');
    expect(field.placeholder).toBe('0');
    expect(field.min).toBe(0);
  });

  test.each([
    'amd_rc',
    'amd_quality',
    'amd_vbaq',
    'amd_smart_access_video',
    'amd_lowlatency_mode',
    'amd_high_motion_quality_boost',
    'amd_av1_screen_content',
    'amd_av1_latency_mode',
  ])('renders %s as an auto-capable select', (settingKey) => {
    const field = getConfigFieldDefinition(settingKey, {
      ...baseContext,
      defaultValue: 'auto',
      currentValue: 'auto',
    });

    expect(field.kind).toBe('select');
    expect(field.options?.map(({ value }) => value)).toContain('auto');
  });
});
