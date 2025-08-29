import { createRouter, createWebHistory, RouteLocationNormalized } from 'vue-router';
import { useAuthStore } from '@/stores/auth';

// Route-level code splitting via dynamic imports
// Each view becomes a separate chunk loaded on demand
const DashboardView = () => import('@/views/DashboardView.vue');
const ApplicationsView = () => import('@/views/ApplicationsView.vue');
const SettingsView = () => import('@/views/SettingsView.vue');
const TroubleshootingView = () => import('@/views/TroubleshootingView.vue');
const ClientManagementView = () => import('@/views/ClientManagementView.vue');

const routes = [
  { path: '/', component: DashboardView },
  { path: '/applications', component: ApplicationsView },
  // Full-bleed settings page
  { path: '/settings', component: SettingsView, meta: { container: 'full' } },
  // Legacy paths (server still routes SPA shell); keep compatibility
  { path: '/config', redirect: '/settings' },
  { path: '/logs', component: DashboardView },
  { path: '/troubleshooting', component: TroubleshootingView },
  { path: '/clients', component: ClientManagementView },
];

export const router = createRouter({
  // Use HTML5 history mode (no # in URLs)
  history: createWebHistory('/'),
  routes,
});

// Lightweight guard: if navigating to a protected route and not authenticated,
// open login modal (in-memory redirect) but allow navigation so URL stays.
router.beforeEach((to: RouteLocationNormalized) => {
  if (typeof window === 'undefined') return true;
  try {
    const auth = useAuthStore();
    if (!auth.isAuthenticated) {
      auth.requireLogin(to.fullPath || to.path);
    }
  } catch (_) {}
  return true;
});
