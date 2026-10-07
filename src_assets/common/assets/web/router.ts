import { createRouter, createWebHistory } from 'vue-router';
import OverviewView from '@/views/OverviewView.vue';

const ApplicationView = () => import('@/views/ApplicationView.vue');
const ApiTokensView = () => import('@/views/ApiTokensView.vue');
const BrowserStreamView = () => import('@/views/BrowserStreamView.vue');
const DevicesView = () => import('@/views/DevicesView.vue');
const IntegrationsView = () => import('@/views/IntegrationsView.vue');
const LibraryView = () => import('@/views/LibraryView.vue');
const LogsView = () => import('@/views/LogsView.vue');
const MaintenanceView = () => import('@/views/MaintenanceView.vue');
const NotFoundView = () => import('@/views/NotFoundView.vue');
const PairView = () => import('@/views/PairView.vue');
const SettingsView = () => import('@/views/SettingsView.vue');
const StatsView = () => import('@/views/StatsView.vue');

const router = createRouter({
  history: createWebHistory('/v2/'),
  routes: [
    {
      path: '/',
      name: 'overview',
      component: OverviewView,
      meta: { titleKey: 'ui.nav.overview' },
    },
    {
      path: '/stream',
      name: 'browser-stream',
      component: BrowserStreamView,
      meta: { titleKey: 'ui.nav.browser_stream' },
    },
    {
      path: '/webrtc',
      redirect: '/stream',
    },
    {
      path: '/library',
      name: 'library',
      component: LibraryView,
      meta: { titleKey: 'ui.nav.library' },
    },
    {
      path: '/library/new',
      name: 'application-new',
      component: ApplicationView,
      meta: { titleKey: 'ui.application.page.addTitle' },
    },
    {
      path: '/library/:id',
      name: 'application',
      component: ApplicationView,
      meta: { titleKey: 'ui.application.page.fallbackTitle' },
    },
    {
      path: '/devices',
      name: 'devices',
      component: DevicesView,
      meta: { titleKey: 'ui.nav.devices' },
    },
    {
      path: '/pair',
      name: 'pair',
      component: PairView,
      meta: { titleKey: 'ui.pair.page.title' },
    },
    {
      path: '/stats',
      name: 'stats',
      component: StatsView,
      meta: { titleKey: 'ui.nav.stats' },
    },
    {
      path: '/sessions',
      redirect: '/stats',
    },
    {
      path: '/integrations',
      name: 'integrations',
      component: IntegrationsView,
      meta: { titleKey: 'ui.nav.integrations' },
    },
    {
      path: '/api-tokens',
      name: 'api-tokens',
      component: ApiTokensView,
      meta: { titleKey: 'ui.nav.api_tokens' },
    },
    {
      path: '/logs',
      name: 'logs',
      component: LogsView,
      meta: { titleKey: 'ui.nav.logs' },
    },
    {
      path: '/settings',
      name: 'settings',
      component: SettingsView,
      meta: { titleKey: 'ui.nav.settings' },
    },
    {
      path: '/maintenance',
      name: 'maintenance',
      component: MaintenanceView,
      meta: { titleKey: 'ui.nav.maintenance' },
    },
    {
      path: '/:pathMatch(.*)*',
      name: 'not-found',
      component: NotFoundView,
      meta: { titleKey: 'ui.not_found.title' },
    },
  ],
  scrollBehavior(to, from, savedPosition) {
    if (savedPosition) return savedPosition;
    if (to.hash && to.path !== '/settings') return { el: to.hash, top: 24 };
    if (to.path === from.path) return undefined;
    return { top: 0 };
  },
});

export default router;
