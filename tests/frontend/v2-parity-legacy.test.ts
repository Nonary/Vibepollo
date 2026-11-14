import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { mount, shallowMount } from '@vue/test-utils';
import { createPinia, setActivePinia } from 'pinia';
import { createI18n } from 'vue-i18n';
import { NCheckbox } from 'naive-ui';
import messagesEn from '@/public/assets/locale/en.json';
import AudioVideo from '@/configs/tabs/AudioVideo.vue';
import DisplayDeviceOptions from '@/configs/tabs/audiovideo/DisplayDeviceOptions.vue';
import { NInput, NButton } from 'naive-ui';
import FrameLimiterStep from '@/configs/tabs/audiovideo/FrameLimiterStep.vue';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import PlatformLayout from '@/PlatformLayout.vue';
import { useConfigStore } from '@/stores/config';
import { http } from '@/http';

function setupConfig(config: Record<string, unknown>, platform = 'linux') {
  const pinia = createPinia();
  setActivePinia(pinia);
  const store = useConfigStore();
  store.metadata = { platform };
  store.setConfig(config);
  return { store, pinia };
}

function plugins(pinia: ReturnType<typeof createPinia>) {
  return [
    pinia,
    createI18n({ legacy: false, locale: 'en', messages: { en: messagesEn } }),
  ];
}

beforeEach(() => {
  vi.spyOn(http, 'get').mockResolvedValue({ status: 200, data: {} } as never);
});

afterEach(() => {
  vi.restoreAllMocks();
  setActivePinia(undefined);
});

describe('legacy configuration behavior', () => {
  it('renders the Wayland HDR checkbox only for Linux and writes its real config model', async () => {
    const { store, pinia } = setupConfig({ virtual_display_mode: 'per_client' });
    const wrapper = mount(AudioVideo, {
      global: {
        plugins: plugins(pinia),
        provide: { i18n: { t: (key: string) => key } },
        stubs: {
          PlatformLayout: false,
          ConfigFieldRenderer: true,
          AdapterNameSelector: true,
          DisplayOutputSelector: true,
          DisplayDeviceOptions: true,
          DisplayModesSettings: true,
          FrameLimiterStep: true,
        },
      },
    });

    const checkbox = wrapper.findAllComponents(NCheckbox).find((item) => item.text().includes('Wayland HDR compatibility'));
    expect(checkbox).toBeDefined();
    await checkbox.vm.$emit('update:checked', true);
    expect(store.config.wayland_hdr_compatibility).toBe(true);
    await wrapper.unmount();

    const { store: windowsStore, pinia: windowsPinia } = setupConfig({ virtual_display_mode: 'per_client' }, 'windows');
    const windows = mount(AudioVideo, {
      global: {
        plugins: plugins(windowsPinia),
        provide: { i18n: { t: (key: string) => key } },
        stubs: {
          PlatformLayout: false,
          ConfigFieldRenderer: true,
          AdapterNameSelector: true,
          DisplayOutputSelector: true,
          DisplayDeviceOptions: true,
          DisplayModesSettings: true,
          FrameLimiterStep: true,
        },
      },
    });
    expect(windows.findAllComponents(NCheckbox).some((item) => item.text().includes('Wayland HDR compatibility'))).toBe(false);
    expect(windowsStore.config.wayland_hdr_compatibility).toBe(false);
    await windows.unmount();
  });

  it('renders Linux limiter choices and connects provider and MangoHUD method controls to config', async () => {
    const { store, pinia } = setupConfig({
      frame_limiter_enable: true,
      frame_limiter_provider: 'auto',
      mangohud_limiter_method: 'late',
      virtual_display_mode: 'per_client',
    });
    const wrapper = shallowMount(FrameLimiterStep, {
      props: { stepLabel: 'Step 4' },
      global: {
        plugins: plugins(pinia),
        stubs: {
          ConfigFieldRenderer: false,
        },
      },
    });

    const provider = wrapper.findAllComponents(ConfigFieldRenderer).find((field) =>
      field.props('settingKey') === 'frame_limiter_provider',
    );
    expect(provider).toBeDefined();
    expect(provider?.props('options').map((option: { value: string }) => option.value)).toEqual([
      'auto',
      'mangohud-proton',
      'proton',
      'mangohud',
      'none',
    ]);
    await provider?.vm.$emit('update:modelValue', 'mangohud');
    expect(store.config.frame_limiter_provider).toBe('mangohud');

    const method = wrapper.findAllComponents(ConfigFieldRenderer).find((field) =>
      field.props('settingKey') === 'mangohud_limiter_method',
    );
    expect(method).toBeDefined();
    const methodOptions = method?.props('options');
    expect(methodOptions.map((option: { value: string; label: string }) => option.value)).toEqual([
      'early',
      'late',
    ]);
    expect(methodOptions.every((option: { label: string }) => option.label.length > 0)).toBe(true);
    await method?.vm.$emit('update:modelValue', 'early');
    expect(store.config.mangohud_limiter_method).toBe('early');
    await wrapper.unmount();
  });
});


describe('legacy display overrides', () => {
  for (const platform of ['linux', 'windows']) {
    it(`renders, edits and removes display overrides on ${platform}`, async () => {
      const { store, pinia } = setupConfig({ dd_configuration_option: 'verify_only' }, platform);
      const wrapper = mount(DisplayDeviceOptions, {
        props: { section: 'options' },
        global: { plugins: plugins(pinia) },
      });
      expect(wrapper.text()).toContain('Display overrides');
      const add = wrapper.findAllComponents(NButton).find((button) => button.text().includes('Add'));
      expect(add).toBeDefined();
      await add!.trigger('click');
      const inputs = wrapper.find('.remap-row').findAllComponents(NInput);
      expect(inputs).toHaveLength(4);
      await inputs[0].vm.$emit('update:value', '1920x1080');
      await inputs[2].vm.$emit('update:value', '3840x2160');
      await inputs[3].vm.$emit('update:value', '120');
      expect(store.config.dd_mode_remapping.mixed).toEqual([{
        requested_resolution: '1920x1080', requested_fps: '',
        final_resolution: '3840x2160', final_refresh_rate: '120',
      }]);
      expect(wrapper.find('#dd_resolution_option').exists()).toBe(platform === 'windows');
      await wrapper.find('.remap-row').findComponent(NButton).trigger('click');
      expect(store.config.dd_mode_remapping.mixed).toEqual([]);
      await wrapper.setProps({ section: 'pre' });
      expect(wrapper.find('.remap-row').exists()).toBe(false);
      await wrapper.setProps({ section: 'options' });
      store.config.dd_configuration_option = 'disabled';
      await wrapper.vm.$nextTick();
      expect(wrapper.text()).toContain('Display overrides will not apply while display automation is disabled');
      expect(wrapper.findAllComponents(NButton).some((button) => button.text().includes('Add'))).toBe(true);
      if (platform === 'linux') {
        const enable = wrapper.findAllComponents(NButton).find((button) => button.text().includes('Enable display automation'))!;
        await enable.trigger('click');
        expect(store.config.dd_configuration_option).toBe('verify_only');
      }
      wrapper.unmount();
    });
  }

  it('edits the policy used when refresh is fixed', async () => {
    const { store, pinia } = setupConfig({
      dd_configuration_option: 'verify_only',
      dd_resolution_option: 'auto',
      dd_refresh_rate_option: 'manual',
    });
    const wrapper = mount(DisplayDeviceOptions, {
      props: { section: 'options' },
      global: { plugins: plugins(pinia) },
    });
    const add = wrapper.findAllComponents(NButton).find((button) => button.text().includes('Add'))!;
    await add.trigger('click');
    const inputs = wrapper.find('.remap-row').findAllComponents(NInput);
    expect(inputs).toHaveLength(2);
    await inputs[0].vm.$emit('update:value', '2560x1440');
    await inputs[1].vm.$emit('update:value', '3840x2160');
    expect(store.config.dd_mode_remapping.resolution_only).toEqual([
      { requested_resolution: '2560x1440', final_resolution: '3840x2160' },
    ]);
    expect(store.config.dd_mode_remapping.mixed).toEqual([]);
    wrapper.unmount();
  });
});
