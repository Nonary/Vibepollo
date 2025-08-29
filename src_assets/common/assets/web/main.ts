import { createApp, ref, watch, App as VueApp } from 'vue';
import { createPinia } from 'pinia';
import { initApp } from '@/init';
import { router } from '@/router';
import App from '@/App.vue';
import './styles/tailwind.css';
import { initHttpLayer } from '@/http';
// Load only the Font Awesome subsets we use (solid + brands)
import '@fortawesome/fontawesome-free/css/solid.min.css';
import '@fortawesome/fontawesome-free/css/brands.min.css';
import { useAuthStore } from '@/stores/auth';
import { useAppsStore } from '@/stores/apps';
import { useConfigStore } from '@/stores/config';
import { useConnectivityStore } from '@/stores/connectivity';
import { ensureLocaleLoaded } from '@/locale-manager';

// Core application instance & stores
const app: VueApp<Element> = createApp(App);
const pinia = createPinia();
app.use(pinia);
app.use(router);

// Expose platform ref early (updated after config load)
const platformRef = ref('');
app.provide('platform', platformRef);

// Central bootstrap: initialize i18n + auth status, then when authenticated load
// config & apps exactly once. Subsequent logouts (401) will re-trigger login modal
// and a later successful login will re-load fresh data.
initApp(app, async () => {
  await initHttpLayer();
  // Start connectivity heartbeat early so we can detect server loss
  const connectivity = useConnectivityStore();
  connectivity.start();

  const auth = useAuthStore();
  const configStore = useConfigStore();
  const appsStore = useAppsStore();

  // Keep provided platform ref in sync with store metadata for any consumers
  watch(
    () => configStore.metadata.platform,
    (p) => {
      platformRef.value = p || '';
    },
    { immediate: true },
  );

  // Initialize auth status from server
  await auth.init();

  auth.waitForAuthentication().then(async () => {
    await configStore.fetchConfig(true);
    // React to locale setting changes by switching i18n at runtime
    watch(
      () => configStore.config?.locale,
      async (loc) => {
        const locale = (loc as any) ?? 'en';
        await ensureLocaleLoaded(locale);
      },
      { immediate: true },
    );
    await appsStore.loadApps(true);
  });

  // Prefetch common route chunks (settings, applications) after idle to improve UX
  try {
    const prefetch = () => {
      // Trigger dynamic imports; browser caches chunks for next navigation
      import('@/views/SettingsView.vue');
      import('@/views/ApplicationsView.vue');
    };
    // Use requestIdleCallback when available to avoid competing with critical work
    if (typeof (window as any).requestIdleCallback === 'function') {
      (window as any).requestIdleCallback(prefetch, { timeout: 2000 });
    } else {
      setTimeout(prefetch, 1500);
    }
  } catch {
    // ignore prefetch errors
  }
});
