import { createRouter, createWebHistory } from 'vue-router';

const router = createRouter({
  history: createWebHistory(),
  routes: [
    {
      path: '/',
      name: 'overview',
      component: () => import('@/views/OverviewView.vue'),
      meta: { titleKey: 'ui.nav.overview' },
    },
    {
      path: '/stream',
      name: 'browser-stream',
      component: () => import('@/views/BrowserStreamView.vue'),
      meta: { titleKey: 'ui.nav.browser_stream' },
    },
    {
      path: '/webrtc',
      redirect: '/stream',
    },
    {
      path: '/library',
      name: 'library',
      component: () => import('@/views/LibraryView.vue'),
      meta: { titleKey: 'ui.nav.library' },
    },
    {
      path: '/library/new',
      name: 'application-new',
      component: () => import('@/views/ApplicationView.vue'),
      meta: { titleKey: 'ui.application.page.addTitle' },
    },
    {
      path: '/library/:id',
      name: 'application',
      component: () => import('@/views/ApplicationView.vue'),
      meta: { titleKey: 'ui.application.page.fallbackTitle' },
    },
    {
      path: '/devices',
      name: 'devices',
      component: () => import('@/views/DevicesView.vue'),
      meta: { titleKey: 'ui.nav.devices' },
    },
    {
      path: '/pair',
      name: 'pair',
      component: () => import('@/views/PairView.vue'),
      meta: { titleKey: 'ui.pair.page.title' },
    },
    {
      path: '/sessions',
      name: 'sessions',
      component: () => import('@/views/SessionsView.vue'),
      meta: { titleKey: 'ui.nav.sessions' },
    },
    {
      path: '/stats',
      name: 'stats',
      component: () => import('@/views/StatsView.vue'),
      meta: { titleKey: 'ui.nav.stats' },
    },
    {
      path: '/integrations',
      name: 'integrations',
      component: () => import('@/views/IntegrationsView.vue'),
      meta: { titleKey: 'ui.nav.integrations' },
    },
    {
      path: '/logs',
      name: 'logs',
      component: () => import('@/views/LogsView.vue'),
      meta: { titleKey: 'ui.nav.logs' },
    },
    {
      path: '/settings',
      name: 'settings',
      component: () => import('@/views/SettingsView.vue'),
      meta: { titleKey: 'ui.nav.settings' },
    },
    {
      path: '/maintenance',
      name: 'maintenance',
      component: () => import('@/views/MaintenanceView.vue'),
      meta: { titleKey: 'ui.nav.maintenance' },
    },
    {
      path: '/:pathMatch(.*)*',
      name: 'not-found',
      component: () => import('@/views/NotFoundView.vue'),
      meta: { titleKey: 'ui.not_found.title' },
    },
  ],
  scrollBehavior(to, from, savedPosition) {
    if (savedPosition) return savedPosition;
    if (to.path === from.path) return undefined;
    return { top: 0 };
  },
});

export default router;
