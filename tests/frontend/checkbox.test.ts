import { mount } from '@vue/test-utils';
import Checkbox from '@web/Checkbox.vue';

describe('Checkbox.vue', () => {
  const mountWith = (model: any, props: any = {}) =>
    mount(Checkbox as any, {
      props: { id: 'flag', localePrefix: 'playnite', label: 'Label', modelValue: model, ...props },
      global: { mocks: { $t: (k: string) => k } },
    });

  test('maps boolean model to true/false values', async () => {
    const w = mountWith(true);
    const input = w.get('input[type="checkbox"]');
    expect((input.element as HTMLInputElement).checked).toBe(true);
    await input.setValue(false);
    expect(w.emitted()['update:modelValue'][0][0]).toBe(false);
  });

  test('maps string "enabled/disabled" and respects inverseValues', async () => {
    const w = mountWith('enabled', { inverseValues: true });
    const input = w.get('input');
    // inverseValues flips truthy/falsy mapping; enabled becomes falsy
    expect((input.element as HTMLInputElement).checked).toBe(false);
    await input.setValue(true);
    // when checked, model updates to mapped truthy (which is original falsy due to inverse)
    expect(w.emitted()['update:modelValue'][0][0]).toBe('disabled');
  });

  test('numeric 1/0 mapping works', async () => {
    const w = mountWith(1);
    const input = w.get('input');
    expect((input.element as HTMLInputElement).checked).toBe(true);
    await input.setValue(false);
    expect(w.emitted()['update:modelValue'][0][0]).toBe(0);
  });

  test('shows default value hint based on `default` prop', () => {
    const w = mountWith(true, { default: 'enabled' });
    expect(w.text()).toContain('_common.enabled_def_cbox');
  });
});
