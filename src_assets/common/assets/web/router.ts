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
  { path: '/settings', component: SettingsView, meta: { container: 'lg' } },
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
router.beforeEach(async (_to: RouteLocationNormalized) => {
  if (typeof window === 'undefined') return true;
  try {
    const auth = useAuthStore();
    // Ensure auth store initialized before route components mount
    if (!auth.ready && typeof auth.init === 'function') {
      try {
        await auth.init();
      } catch {
        /* ignore */
      }
    }
    // If not authenticated, trigger overlay (do not redirect)
    if (!auth.isAuthenticated) auth.requireLogin();
  } catch {
    /* ignore */
  }
  // Always allow navigation so URL remains intact
  return true;
});
