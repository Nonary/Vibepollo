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
      const field = getConfigFieldDefinition('back_button_timeout', {
        ...baseContext,
        currentValue,
        defaultValue: -1,
      });

      expect(field.kind).toBe('number');
    },
  );

  test('still infers checkbox fields from boolean-like numeric values when not overridden', () => {
    const field = getConfigFieldDefinition('custom_toggle', {
      ...baseContext,
      currentValue: 0,
    });

    expect(field.kind).toBe('checkbox');
  });
});
