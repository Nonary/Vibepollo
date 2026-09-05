<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute } from 'vue-router';

import { UiIcon } from '@/components/ui';
import { useSystemStore } from '@/stores/system';

const system = useSystemStore();
const route = useRoute();
const { t } = useI18n();
const baseUrl = import.meta.env.BASE_URL;
const mobileQuery = window.matchMedia('(max-width: 767px)');
const compactQuery = window.matchMedia('(max-width: 1023px)');
const isMobile = ref(mobileQuery.matches);
const isCompact = ref(compactQuery.matches);
const compactNavigation = computed(
  () => !isMobile.value && (isCompact.value || system.navCollapsed),
);

const navigationGroups = [
  {
    labelKey: 'ui.shell.workspace',
    items: [
      { labelKey: 'ui.nav.overview', icon: 'overview', to: '/' },
      { labelKey: 'ui.nav.library', icon: 'library', to: '/library' },
      { labelKey: 'ui.nav.devices', icon: 'devices', to: '/devices' },
      { labelKey: 'ui.nav.browser_stream', icon: 'play', to: '/stream' },
    ],
  },
  {
    labelKey: 'ui.shell.monitor',
    items: [
      { labelKey: 'ui.nav.stats', icon: 'activity', to: '/stats' },
      { labelKey: 'ui.nav.logs', icon: 'logs', to: '/logs' },
    ],
  },
  {
    labelKey: 'ui.shell.manage',
    items: [
      { labelKey: 'ui.nav.settings', icon: 'settings', to: '/settings' },
      { labelKey: 'ui.nav.integrations', icon: 'integrations', to: '/integrations' },
      { labelKey: 'ui.nav.api_tokens', icon: 'key', to: '/api-tokens' },
      { labelKey: 'ui.nav.maintenance', icon: 'help', to: '/maintenance' },
    ],
  },
] as const;
const themes = [
  { value: 'light', icon: 'sun' },
  { value: 'dark', icon: 'moon' },
  { value: 'auto', icon: 'devices' },
] as const;

const statusText = computed(() => {
  if (system.health === 'unknown') return t('ui.settings.linux.states.unknown');
  if (system.health === 'warning') return t('ui.status.needs_attention');
  if (system.health === 'streaming') return t('ui.status.streaming');
  return t('ui.status.ready');
});
const statusIcon = computed(() => {
  if (system.health === 'unknown') return 'info';
  if (system.health === 'warning') return 'alert-triangle';
  if (system.health === 'streaming') return 'activity';
  return 'check-circle';
});

function isCurrent(path: string): boolean {
  if (path === '/') return route.path === '/';
  if (path === '/devices' && route.path === '/pair') return true;
  return route.path.startsWith(path);
}
function closeMobileNavigation(): void {
  system.mobileNavOpen = false;
}
function onViewportChange(): void {
  isMobile.value = mobileQuery.matches;
  isCompact.value = compactQuery.matches;
  if (!isMobile.value) closeMobileNavigation();
}
function onKeydown(event: KeyboardEvent): void {
  if (!system.mobileNavOpen) return;
  if (event.key === 'Escape') closeMobileNavigation();
  if (event.key === 'Tab') {
    const elements = Array.from(
      navigation.value?.querySelectorAll<HTMLElement>('a[href], button:not(:disabled)') ?? [],
    ).filter((element) => element.getClientRects().length);
    const first = elements[0],
      last = elements.at(-1);
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last?.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first?.focus();
    }
  }
}

watch(() => route.fullPath, closeMobileNavigation);
let hostTimer: number | undefined;
const navigation = ref<HTMLElement | null>(null);
let navInvoker: HTMLElement | null = null;
let previousOverflow: string | null = null;
function restoreScroll(): void {
  if (previousOverflow !== null) {
    document.body.style.overflow = previousOverflow;
    previousOverflow = null;
  }
}
watch(
  () => system.mobileNavOpen,
  async (open) => {
    if (open) {
      navInvoker = document.activeElement as HTMLElement;
      previousOverflow = document.body.style.overflow;
      document.body.style.overflow = 'hidden';
      await nextTick();
      navigation.value?.querySelector<HTMLElement>('button, a')?.focus();
    } else {
      restoreScroll();
      await nextTick();
      if (navInvoker?.getClientRects().length) navInvoker.focus();
    }
  },
);
onMounted(() => {
  window.addEventListener('keydown', onKeydown);
  mobileQuery.addEventListener('change', onViewportChange);
  compactQuery.addEventListener('change', onViewportChange);
  hostTimer = window.setInterval(() => {
    if (!document.hidden) void system.refreshHost();
  }, 10000);
});
onBeforeUnmount(() => {
  window.removeEventListener('keydown', onKeydown);
  mobileQuery.removeEventListener('change', onViewportChange);
  compactQuery.removeEventListener('change', onViewportChange);
  window.clearInterval(hostTimer);
  restoreScroll();
});
</script>

<template>
  <div
    class="app-shell"
    :class="{
      'app-shell--collapsed': compactNavigation,
      'app-shell--nav-open': system.mobileNavOpen,
    }"
  >
    <a class="vs-skip-link" href="#main-content" :inert="system.mobileNavOpen || undefined">{{
      t('ui.app.skip_to_content')
    }}</a>
    <header class="mobile-bar" :inert="system.mobileNavOpen || undefined">
      <button
        class="icon-button"
        type="button"
        :aria-label="t('ui.shell.open_navigation')"
        :aria-expanded="system.mobileNavOpen"
        aria-controls="app-navigation"
        @click="system.mobileNavOpen = true"
      >
        <UiIcon name="menu" :size="20" />
      </button>
      <RouterLink class="mobile-brand" to="/" :aria-label="t('ui.shell.brand_overview')">
        <img :src="`${baseUrl}images/logo-apollo-45.png`" alt="" width="28" height="28" />
        <span>Vibepollo</span>
      </RouterLink>
      <span class="mobile-status" :data-state="system.health">{{ statusText }}</span>
    </header>

    <button
      v-if="system.mobileNavOpen"
      class="nav-scrim"
      type="button"
      tabindex="-1"
      :aria-label="t('ui.shell.close_navigation')"
      @click="closeMobileNavigation"
    />

    <aside
      ref="navigation"
      id="app-navigation"
      class="sidebar"
      :inert="(isMobile && !system.mobileNavOpen) || undefined"
      :role="system.mobileNavOpen ? 'dialog' : undefined"
      :aria-modal="system.mobileNavOpen ? true : undefined"
      :aria-label="t('ui.shell.primary_navigation')"
    >
      <div class="sidebar__header">
        <RouterLink class="brand" to="/" :aria-label="t('ui.shell.brand_overview')">
          <img :src="`${baseUrl}images/logo-apollo-45.png`" alt="" width="32" height="32" />
          <span class="sidebar__label">
            <span class="brand__name">Vibepollo</span>
            <span class="brand__description">{{ t('ui.shell.host_console') }}</span>
          </span>
        </RouterLink>
        <button
          class="icon-button sidebar__close"
          type="button"
          :aria-label="t('ui.shell.close_navigation')"
          @click="closeMobileNavigation"
        >
          <UiIcon name="x" :size="20" />
        </button>
      </div>

      <nav class="sidebar__navigation">
        <div v-for="group in navigationGroups" :key="group.labelKey" class="nav-group">
          <p class="nav-group__label sidebar__label">{{ t(group.labelKey) }}</p>
          <ul class="nav-list">
            <li v-for="item in group.items" :key="item.to">
              <RouterLink
                class="nav-link"
                :class="{ 'nav-link--current': isCurrent(item.to) }"
                :to="item.to"
                :aria-current="isCurrent(item.to) ? 'page' : undefined"
                :title="t(item.labelKey)"
              >
                <UiIcon :name="item.icon" :size="18" />
                <span class="sidebar__label">{{ t(item.labelKey) }}</span>
              </RouterLink>
            </li>
          </ul>
        </div>
      </nav>

      <div class="sidebar__footer">
        <button
          class="host-health"
          :data-state="system.health"
          type="button"
          :title="t('ui.shell.refresh_host_status')"
          :aria-busy="system.loadingHost"
          @click="system.refreshHost"
        >
          <UiIcon :name="statusIcon" :size="18" />
          <span class="sidebar__label">
            <strong>{{ statusText }}</strong>
            <small>{{ system.metadata?.version || t('ui.shell.host_status') }}</small>
          </span>
          <UiIcon name="refresh" :size="14" class="host-health__refresh sidebar__label" />
        </button>

        <div class="theme-picker">
          <span class="sidebar__label">{{ t('ui.shell.appearance') }}</span>
          <div class="theme-picker__options" role="group" :aria-label="t('ui.shell.appearance')">
            <button
              v-for="theme in themes"
              :key="theme.value"
              class="icon-button"
              type="button"
              :aria-label="t(`ui.shell.theme_${theme.value}`)"
              :title="t(`ui.shell.theme_${theme.value}`)"
              :aria-pressed="system.theme === theme.value"
              @click="system.setTheme(theme.value)"
            >
              <UiIcon :name="theme.icon" :size="16" />
            </button>
          </div>
        </div>
        <a class="sidebar__legacy" href="/" :title="t('ui.shell.classic_interface')">
          <UiIcon name="external-link" :size="16" />
          <span class="sidebar__label">{{ t('ui.shell.classic_interface') }}</span>
        </a>
        <div class="sidebar__utility">
          <button
            class="sidebar__logout"
            type="button"
            :title="t('navbar.logout')"
            @click="system.logout"
          >
            <UiIcon name="logout" :size="16" />
            <span class="sidebar__label">{{ t('navbar.logout') }}</span>
          </button>
          <button
            class="icon-button sidebar__collapse"
            type="button"
            :aria-label="
              t(system.navCollapsed ? 'ui.shell.expand_navigation' : 'ui.shell.collapse_navigation')
            "
            :title="
              t(system.navCollapsed ? 'ui.shell.expand_navigation' : 'ui.shell.collapse_navigation')
            "
            @click="system.toggleNav"
          >
            <UiIcon :name="system.navCollapsed ? 'chevron-right' : 'chevron-left'" />
          </button>
        </div>
      </div>
    </aside>
    <main
      :inert="system.mobileNavOpen || undefined"
      id="main-content"
      class="main-content"
      tabindex="-1"
    >
      <slot />
    </main>
  </div>
</template>
