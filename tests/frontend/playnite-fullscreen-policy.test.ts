import { afterEach, describe, expect, it, vi } from 'vitest';
import { flushPromises, shallowMount } from '@vue/test-utils';
import { createPinia, setActivePinia } from 'pinia';
import { createI18n } from 'vue-i18n';
import AppEditModal from '@/components/AppEditModal.vue';
import type { ServerApp } from '@/components/app-edit/types';
import { http } from '@/http';
import messagesEn from '@/public/assets/locale/en.json';
import { useConfigStore } from '@/stores/config';
import { preservePlayniteFullscreen } from '@/utils/playniteFullscreenPolicy';

vi.mock('naive-ui', async () => ({
  ...((await vi.importActual('naive-ui')) as Record<string, unknown>),
  useMessage: () => ({ error: vi.fn() }),
}));

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  setActivePinia(undefined);
});

describe('legacy editor Playnite fullscreen metadata', () => {
  it.each([true, false, 'true', 1, null])(
    'retains an existing value without normalization: %s',
    (flag) => {
      const source = { 'playnite-fullscreen': flag };
      const payload: Record<string, unknown> = {
        name: 'Playnite (Fullscreen)',
        'wait-all': false,
      };
      preservePlayniteFullscreen(source, payload);
      expect(payload['playnite-fullscreen']).toBe(flag);
      expect(source['playnite-fullscreen']).toBe(flag);
      expect(payload['wait-all']).toBe(false);
    },
  );

  it('does not add the flag for absent, null, or inherited metadata', () => {
    for (const source of [{}, null, undefined, Object.create({ 'playnite-fullscreen': true })]) {
      const payload: Record<string, unknown> = { name: 'Desktop' };
      preservePlayniteFullscreen(source, payload);
      expect(payload).toEqual({ name: 'Desktop' });
    }
  });

  it.each([true, false])(
    'hydrates and saves the selected app without losing fullscreen=%s',
    async (flag) => {
      vi.stubGlobal('requestAnimationFrame', () => 0);
      vi.spyOn(http, 'get').mockResolvedValue({
        status: 200,
        data: {},
      } as never);
      const post = vi.spyOn(http, 'post').mockResolvedValue({
        status: 200,
        data: { status: true },
      } as never);
      const pinia = createPinia();
      setActivePinia(pinia);
      useConfigStore().metadata = { platform: 'windows' };
      const app: ServerApp = {
        name: 'Playnite (Fullscreen)',
        uuid: 'f6169d78-43c2-454f-aa42-96ab580dcf97',
        'playnite-fullscreen': flag,
        'wait-all': true,
      };
      // Child controls and HTTP are mocked; hydration, serialization and save are real.
      const wrapper = shallowMount(AppEditModal, {
        props: { modelValue: false, app },
        global: {
          plugins: [
            pinia,
            createI18n({
              legacy: false,
              locale: 'en',
              messages: { en: messagesEn },
            }),
          ],
          stubs: { NInput: true },
        },
      });
      try {
        await wrapper.setProps({ modelValue: true });
        await flushPromises();
        const state = (wrapper.vm as any).$.setupState;
        expect(state.form.waitAll).toBe(true);
        state.form.waitAll = false;
        await state.save();
        const saves = () => post.mock.calls.filter(([url]) => url === './api/apps');
        expect(saves()).toHaveLength(1);
        const payload = saves()[0][1] as Record<string, unknown>;
        expect(payload.uuid).toBe(app.uuid);
        expect(payload['wait-all']).toBe(false);
        expect(JSON.parse(JSON.stringify(payload))['playnite-fullscreen']).toBe(flag);
        expect(app['playnite-fullscreen']).toBe(flag);
        expect(wrapper.emitted('saved')).toHaveLength(1);

        // Reusing the modal for an ordinary or new app must not leak the old flag.
        for (const nextApp of [{ name: 'Desktop', uuid: 'other' }, null]) {
          await wrapper.setProps({ modelValue: false, app: nextApp });
          await wrapper.setProps({ modelValue: true });
          await flushPromises();
          const previousSaveCount = saves().length;
          await state.save();
          expect(saves()).toHaveLength(previousSaveCount + 1);
          expect(saves().slice(-1)[0][1]).not.toHaveProperty('playnite-fullscreen');
        }
      } finally {
        wrapper.unmount();
      }
    },
  );
});
