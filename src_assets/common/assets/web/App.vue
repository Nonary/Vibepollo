<template>
  <n-config-provider :theme="isDark ? darkTheme : null" :theme-overrides="naiveOverrides">
    <n-loading-bar-provider>
      <n-dialog-provider>
        <n-notification-provider>
          <n-message-provider>
            <div class="min-h-screen flex flex-col bg-light dark:bg-dark text-dark dark:text-light">
              <header
                class="sticky top-0 z-30 h-14 flex items-center gap-4 px-4 border-b border-dark/10 dark:border-light/10 bg-light/70 dark:bg-dark/60 backdrop-blur supports-[backdrop-filter]:bg-light/40 supports-[backdrop-filter]:dark:bg-dark/40"
              >
                <div class="flex items-center gap-3 min-w-0">
                  <img src="/images/logo-apollo-45.png" alt="Vibepollo" class="h-8 w-8" />
                  <h1 class="text-base md:text-lg font-semibold tracking-tight truncate">
                    {{
                      displayTitle && displayTitle.includes('.') ? $t(displayTitle) : displayTitle
                    }}
                  </h1>
                </div>
                <nav class="hidden md:flex items-center gap-1 text-sm font-medium ml-2">
                  <RouterLink to="/" :class="linkClass('/')">
                    <i class="fas fa-gauge" /><span>{{ $t('navbar.home') }}</span>
                  </RouterLink>
                  <RouterLink to="/applications" :class="linkClass('/applications')">
                    <i class="fas fa-table-cells-large" /><span>{{
                      $t('navbar.applications')
                    }}</span>
                  </RouterLink>
                  <RouterLink to="/clients" :class="linkClass('/clients')">
                    <i class="fas fa-users-cog" /><span>{{ $t('clients.nav') }}</span>
                  </RouterLink>
                  <RouterLink to="/settings" :class="linkClass('/settings')">
                    <i class="fas fa-sliders" /><span>{{ $t('navbar.configuration') }}</span>
                  </RouterLink>
                  <RouterLink to="/troubleshooting" :class="linkClass('/troubleshooting')">
                    <i class="fas fa-bug" /><span>{{ $t('navbar.troubleshoot') }}</span>
                  </RouterLink>
                  <a href="#" :class="linkClass('/logout')" @click.prevent="logout">
                    <i class="fas fa-sign-out-alt" /><span>{{ $t('navbar.logout') }}</span>
                  </a>
                </nav>
                <!-- Mobile menu button (md:hidden) -->
                <div class="md:hidden ml-auto flex items-center gap-2">
                  <n-dropdown
                    trigger="click"
                    :show-arrow="true"
                    :options="mobileMenuOptions"
                    @select="onMobileSelect"
                  >
                    <n-button type="primary" strong circle size="small" aria-label="Menu">
                      <i class="fas fa-bars" />
                    </n-button>
                  </n-dropdown>
                  <!-- Show save/status control on mobile app bar when on Settings -->
                  <SavingStatus />
                  <ThemeToggle />
                </div>
                <!-- Desktop actions -->
                <div class="hidden md:flex ml-auto items-center gap-3 text-xs">
                  <SavingStatus />
                  <ThemeToggle />
                </div>
              </header>

              <!-- Content: single shared container around RouterView; width via route meta -->
              <main class="flex-1 overflow-auto">
                <RouterView v-slot="{ Component, route: r }">
                  <div :class="containerClass(r)">
                    <Transition name="fade-fast" mode="out-in">
                      <component :is="Component" />
                    </Transition>
                  </div>
                </RouterView>
              </main>
              <!-- Immediate background for login modal (no transition delay) -->
              <div v-if="loginOverlay" class="fixed inset-0 z-[110]">
                <div
                  class="absolute inset-0 bg-gradient-to-br from-white/70 via-white/60 to-white/70 dark:from-black/70 dark:via-black/60 dark:to-black/70 backdrop-blur-md"
                ></div>
              </div>
              <LoginModal />
              <OfflineOverlay />
              <transition name="fade-fast">
                <div v-if="loggedOut" class="fixed inset-0 z-[120] flex flex-col">
                  <div
                    class="absolute inset-0 bg-gradient-to-br from-white/70 via-white/60 to-white/70 dark:from-black/70 dark:via-black/60 dark:to-black/70 backdrop-blur-md"
                  ></div>
                  <div
                    class="relative flex-1 flex flex-col items-center justify-center p-6 overflow-y-auto"
                  >
                    <div class="w-full max-w-md mx-auto text-center space-y-6">
                      <img
                        src="/images/logo-apollo-45.png"
                        alt="Vibepollo"
                        class="h-24 w-24 opacity-80 mx-auto select-none"
                      />
                      <div class="space-y-2">
                        <h2 class="text-2xl font-semibold tracking-tight">
                          {{ $t('auth.logout_success') }}
                        </h2>
                        <p class="text-sm opacity-80 leading-relaxed">
                          {{ $t('auth.logout_refresh_hint') }}
                        </p>
                      </div>
                      <div class="flex items-center justify-center pt-2">
                        <n-button type="primary" @click="refreshPage">
                          {{ $t('auth.logout_refresh_button') }}
                          <i class="fas fa-rotate" />
                        </n-button>
                      </div>
                      <p class="mt-8 text-[10px] tracking-wider uppercase opacity-60 select-none">
                        Sunshine
                      </p>
                    </div>
                  </div>
                </div>
              </transition>
            </div>
          </n-message-provider>
        </n-notification-provider>
      </n-dialog-provider>
    </n-loading-bar-provider>
  </n-config-provider>
</template>
<script setup lang="ts">
import { ref, watch, computed, h } from 'vue';
import {
  NConfigProvider,
  NDialogProvider,
  NMessageProvider,
  NNotificationProvider,
  NLoadingBarProvider,
  NButton,
  NDropdown,
  darkTheme,
} from 'naive-ui';
import { useNaiveThemeOverrides, useDarkModeClassRef } from '@/naive-theme';
import { useI18n } from 'vue-i18n';
import { useRoute, useRouter } from 'vue-router';
import ThemeToggle from '@/ThemeToggle.vue';
import SavingStatus from '@/components/SavingStatus.vue';
import LoginModal from '@/components/LoginModal.vue';
import OfflineOverlay from '@/components/OfflineOverlay.vue';
import { http } from '@/http';
import { useAuthStore } from './stores/auth';
import { useConfigStore } from '@/stores/config';
import { storeToRefs } from 'pinia';
import { useConnectivityStore } from '@/stores/connectivity';

// Sync Naive theme to existing dark mode class and pick colors from CSS vars
const isDark = useDarkModeClassRef();
const naiveOverrides = useNaiveThemeOverrides();

const route = useRoute();
const router = useRouter();

// Use config metadata as a fallback for container sizing when route meta isn't set
const cfgStore = useConfigStore();
const { metadata } = storeToRefs(cfgStore);

const linkClass = (path: string) => {
  const base = 'inline-flex items-center gap-2 px-3 py-1 rounded-md text-brand';
  const active = route.path === path;
  if (active) return base + ' font-semibold bg-primary/20 text-brand';
  return base + ' hover:bg-primary/10';
};
const pageTitle = ref('Dashboard');
const displayTitle = computed(() => {
  // If pageTitle is an i18n key like 'navbar.troubleshoot', call $t from template via global $t
  // We return the key here; template will call $t when necessary using a heuristic there.
  return pageTitle.value;
});
// app bar only; sidebar removed

watch(
  () => route.path,
  (p) => {
    const map: Record<string, string> = {
      '/': 'navbar.home',
      '/applications': 'navbar.applications',
      '/settings': 'navbar.configuration',
      '/logs': 'navbar.troubleshoot',
      '/troubleshooting': 'navbar.troubleshoot',
      '/clients': 'clients.nav',
    };
    const v = map[p] || 'Sunshine';
    pageTitle.value = v;
  },
  { immediate: true },
);

const loggedOut = ref(false);

// Mirror LoginModal visibility for instant background application
const authForOverlay = useAuthStore();
const loginOverlay = computed(
  () =>
    authForOverlay.ready &&
    authForOverlay.showLoginModal &&
    !authForOverlay.isAuthenticated &&
    !authForOverlay.logoutInitiated,
);

async function logout() {
  const authStore = useAuthStore();
  const connectivity = useConnectivityStore();
  try {
    await http.post('/api/auth/logout', {}, { validateStatus: () => true });
  } catch (e) {
    console.error('Logout failed:', e);
  }
  try {
    (authStore as any).logoutInitiated = true;
  } catch {}
  try {
    authStore.setAuthenticated(false);
  } catch {}
  // Stop background connectivity checks and any other background polling
  try {
    connectivity.stop();
  } catch {}
  loggedOut.value = true;
}

function refreshPage() {
  window.location.reload();
}

const { t } = useI18n();
const mobileMenuOptions = computed(() => {
  const icon = (cls: string) => () => h('i', { class: cls });
  return [
    { label: t('navbar.home'), key: '/', icon: icon('fas fa-gauge') },
    {
      label: t('navbar.applications'),
      key: '/applications',
      icon: icon('fas fa-table-cells-large'),
    },
    { label: t('clients.nav'), key: '/clients', icon: icon('fas fa-users-cog') },
    { label: t('navbar.configuration'), key: '/settings', icon: icon('fas fa-sliders') },
    { label: t('navbar.troubleshoot'), key: '/troubleshooting', icon: icon('fas fa-bug') },
    { type: 'divider' as const },
    { label: t('navbar.logout'), key: '__logout', icon: icon('fas fa-sign-out-alt') },
  ];
});

function onMobileSelect(key: string | number): void {
  if (key === '__logout') {
    void logout();
    return;
  }
  if (typeof key === 'string') router.push(key);
}

// Layout container sizing via route meta: { container: 'sm'|'md'|'lg'|'xl'|'full' }
const base = 'mx-auto px-4 sm:px-6 lg:px-8 py-4 md:py-6';
const sizes: Record<string, string> = {
  sm: 'max-w-2xl',
  md: 'max-w-3xl',
  lg: 'max-w-5xl',
  xl: 'max-w-7xl',
  full: 'max-w-none px-0 sm:px-0 lg:px-0',
};
function containerClass(r: any) {
  const routeSize = r?.meta?.container;
  const size = routeSize ?? (metadata.value as any)?.container ?? 'lg';
  return `${base} ${sizes[size] || sizes['lg']}`;
}
</script>
