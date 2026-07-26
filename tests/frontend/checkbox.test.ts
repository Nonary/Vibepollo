import { mount } from '@vue/test-utils';
import { createI18n } from '@web/node_modules/vue-i18n/dist/vue-i18n.mjs';
import Checkbox from '@web/Checkbox.vue';

describe('Checkbox.vue', () => {
  const mountWith = (model: any, props: any = {}) => {
    const i18n = createI18n({
      legacy: false,
      locale: 'en',
      missingWarn: false,
      fallbackWarn: false,
      messages: { en: {} },
    });

    return mount(Checkbox as any, {
      props: { id: 'flag', localePrefix: 'playnite', label: 'Label', modelValue: model, ...props },
      global: { plugins: [i18n] },
    });
  };

  test('maps boolean model to true/false values', async () => {
    const w = mountWith(true);
    const input = w.get('[role="checkbox"]');
    expect(input.attributes('aria-checked')).toBe('true');
    await input.trigger('click');
    expect(w.emitted()['update:modelValue'][0][0]).toBe(false);
  });

  test('maps string "enabled/disabled" and respects inverseValues', async () => {
    const w = mountWith('enabled', { inverseValues: true });
    const input = w.get('[role="checkbox"]');
    // inverseValues flips truthy/falsy mapping; enabled becomes falsy
    expect(input.attributes('aria-checked')).toBe('false');
    await input.trigger('click');
    // when checked, model updates to mapped truthy (which is original falsy due to inverse)
    expect(w.emitted()['update:modelValue'][0][0]).toBe('disabled');
  });

  test('numeric 1/0 mapping works', async () => {
    const w = mountWith(1);
    const input = w.get('[role="checkbox"]');
    expect(input.attributes('aria-checked')).toBe('true');
    await input.trigger('click');
    expect(w.emitted()['update:modelValue'][0][0]).toBe(0);
  });

  test('shows default value hint based on `default` prop', () => {
    const w = mountWith(true, { default: 'enabled' });
    expect(w.text()).toContain('_common.enabled_def_cbox');
  });

  test('updates translated label and description props after a locale change', async () => {
    const w = mountWith(true, {
      label: 'Enable controller input',
      desc: 'Allow controller input from clients',
    });

    await w.setProps({
      label: 'ゲームパッド入力を有効にする',
      desc: 'クライアントからのコントローラー入力を許可します',
    });

    expect(w.text()).toContain('ゲームパッド入力を有効にする');
    expect(w.text()).toContain('クライアントからのコントローラー入力を許可します');
    expect(w.text()).not.toContain('Enable controller input');
    expect(w.text()).not.toContain('Allow controller input from clients');
  });
});
