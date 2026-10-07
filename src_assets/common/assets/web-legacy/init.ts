import { App } from 'vue';
import i18n from '@/locale';
import { setI18nGlobal } from '@/locale-manager';

export function initApp(
  app: App<Element>,
  config?: (app: App<Element>) => Promise<void> | void,
): void {
  // Wait for locale initialization, then run optional app-level setup (like loading config)
  // If a `config` callback is provided it may be async — run it before mounting so
  // stores and components see the runtime config immediately.
  i18n().then(async (i18n) => {
    app.use(i18n);
    app.provide('i18n', i18n.global);
    // expose i18n instance for runtime locale switching
    setI18nGlobal(i18n);
    if (config) {
      try {
        // allow `config` to be async and wait for it to complete
        await config(app);
      } catch (e) {
        // swallow errors to avoid blocking app entirely
        console.error('initApp: config loader failed', e);
      }
    }
    // Mount after any early initialization
    app.mount('#app');
  });
}
