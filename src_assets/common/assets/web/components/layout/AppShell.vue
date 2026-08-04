<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import { useRoute } from 'vue-router';

import { UiIcon } from '@/components/ui';
import { useSystemStore, type ThemePreference } from '@/stores/system';

const system = useSystemStore();
const route = useRoute();
const { t } = useI18n();

const primaryNavigation = [
  { labelKey: 'ui.nav.overview', icon: 'overview', to: '/' },
  { labelKey: 'ui.nav.browser_stream', icon: 'play', to: '/stream' },
  { labelKey: 'ui.nav.library', icon: 'library', to: '/library' },
  { labelKey: 'ui.nav.devices', icon: 'devices', to: '/devices' },
  { labelKey: 'ui.nav.stats', icon: 'activity', to: '/stats' },
  { labelKey: 'ui.nav.integrations', icon: 'integrations', to: '/integrations' },
  { labelKey: 'ui.nav.logs', icon: 'logs', to: '/logs' },
] as const;

const secondaryNavigation = [
  { labelKey: 'ui.nav.api_tokens', icon: 'key', to: '/api-tokens' },
  { labelKey: 'ui.nav.settings', icon: 'settings', to: '/settings' },
  { labelKey: 'ui.nav.maintenance', icon: 'help', to: '/maintenance' },
] as const;

const statusText = computed(() => {
  if (system.health === 'warning') return t('ui.status.needs_attention');
  if (system.health === 'streaming') return t('ui.status.streaming');
  return t('ui.status.ready');
});

const statusIcon = computed(() => {
  if (system.health === 'warning') return 'alert-triangle';
  if (system.health === 'streaming') return 'activity';
  return 'check-circle';
});

function isCurrent(path: string): boolean {
  if (path === '/') return route.path === '/';
  return route.path.startsWith(path);
}

function setTheme(event: Event): void {
  const value = (event.target as HTMLSelectElement).value;
  if (value === 'auto' || value === 'dark' || value === 'light') {
    system.setTheme(value as ThemePreference);
  }
}

function closeMobileNavigation(): void {
  system.mobileNavOpen = false;
}

function onKeydown(event: KeyboardEvent): void {
  if (event.key === 'Escape' && system.mobileNavOpen) closeMobileNavigation();
}

watch(
  () => route.fullPath,
  () => closeMobileNavigation(),
);

onMounted(() => window.addEventListener('keydown', onKeydown));
onBeforeUnmount(() => window.removeEventListener('keydown', onKeydown));
</script>

<template>
  <div
    class="app-shell"
    :class="{
      'app-shell--collapsed': system.navCollapsed,
      'app-shell--nav-open': system.mobileNavOpen,
    }"
  >
    <header class="mobile-bar">
      <button
        class="icon-button"
        type="button"
        :aria-label="t('ui.shell.open_navigation')"
        :title="t('ui.shell.open_navigation')"
        @click="system.mobileNavOpen = true"
      >
        <UiIcon name="menu" />
      </button>
      <RouterLink class="mobile-brand" to="/" :aria-label="t('ui.shell.brand_overview')">
        <img src="/images/logo-apollo-45.png" alt="" width="28" height="28" />
        <span>Vibepollo</span>
      </RouterLink>
      <span class="mobile-status" :data-state="system.health">{{ statusText }}</span>
    </header>

    <button
      v-if="system.mobileNavOpen"
      class="nav-scrim"
      type="button"
      :aria-label="t('ui.shell.close_navigation')"
      @click="closeMobileNavigation"
    />

    <aside id="app-navigation" class="sidebar" :aria-label="t('ui.shell.primary_navigation')">
      <div class="sidebar__header">
        <RouterLink class="brand" to="/" :aria-label="t('ui.shell.brand_overview')">
          <img src="/images/logo-apollo-45.png" alt="" width="32" height="32" />
          <span class="sidebar__label brand__name">Vibepollo</span>
        </RouterLink>
        <button
          class="icon-button sidebar__close"
          type="button"
          :aria-label="t('ui.shell.close_navigation')"
          :title="t('ui.shell.close_navigation')"
          @click="closeMobileNavigation"
        >
          <UiIcon name="x" />
        </button>
      </div>

      <nav class="sidebar__navigation">
        <ul class="nav-list">
          <li v-for="item in primaryNavigation" :key="item.to">
            <RouterLink
              class="nav-link"
              :class="{ 'nav-link--current': isCurrent(item.to) }"
              :to="item.to"
              :aria-current="isCurrent(item.to) ? 'page' : undefined"
              :title="system.navCollapsed ? t(item.labelKey) : undefined"
            >
              <UiIcon :name="item.icon" :size="20" />
              <span class="sidebar__label">{{ t(item.labelKey) }}</span>
            </RouterLink>
          </li>
        </ul>

        <ul class="nav-list nav-list--secondary">
          <li v-for="item in secondaryNavigation" :key="item.to">
            <RouterLink
              class="nav-link"
              :class="{ 'nav-link--current': isCurrent(item.to) }"
              :to="item.to"
              :aria-current="isCurrent(item.to) ? 'page' : undefined"
              :title="system.navCollapsed ? t(item.labelKey) : undefined"
            >
              <UiIcon :name="item.icon" :size="20" />
              <span class="sidebar__label">{{ t(item.labelKey) }}</span>
            </RouterLink>
          </li>
        </ul>
      </nav>

      <div class="sidebar__footer">
        <button
          class="host-health"
          :data-state="system.health"
          type="button"
          :title="system.navCollapsed ? statusText : t('ui.shell.refresh_host_status')"
          @click="system.refreshHost"
        >
          <UiIcon :name="statusIcon" :size="18" />
          <span class="sidebar__label">
            <strong>{{ statusText }}</strong>
            <small>{{ system.metadata?.version || t('ui.shell.host_status') }}</small>
          </span>
        </button>

        <div class="theme-picker">
          <UiIcon name="settings" :size="18" aria-hidden="true" />
          <label class="sidebar__label" for="appearance-theme">{{
            t('ui.shell.appearance')
          }}</label>
          <select
            id="appearance-theme"
            class="theme-picker__select"
            :value="system.theme"
            :aria-label="t('ui.shell.appearance')"
            :title="t('ui.shell.theme', { theme: t(`ui.shell.theme_${system.theme}`) })"
            @change="setTheme"
          >
            <option value="auto">{{ t('ui.shell.theme_auto') }}</option>
            <option value="light">{{ t('ui.shell.theme_light') }}</option>
            <option value="dark">{{ t('ui.shell.theme_dark') }}</option>
          </select>
        </div>

        <div class="sidebar__utility">
          <button
            class="icon-button sidebar__collapse"
            type="button"
            :aria-label="
              system.navCollapsed
                ? t('ui.shell.expand_navigation')
                : t('ui.shell.collapse_navigation')
            "
            :title="
              system.navCollapsed
                ? t('ui.shell.expand_navigation')
                : t('ui.shell.collapse_navigation')
            "
            @click="system.toggleNav"
          >
            <UiIcon :name="system.navCollapsed ? 'chevron-right' : 'chevron-left'" />
          </button>
          <button
            class="icon-button"
            type="button"
            :aria-label="t('navbar.logout')"
            :title="t('navbar.logout')"
            @click="system.logout"
          >
            <UiIcon name="user" />
          </button>
        </div>
      </div>
    </aside>

    <main id="main-content" class="main-content" tabindex="-1">
      <slot />
    </main>
  </div>
</template>
