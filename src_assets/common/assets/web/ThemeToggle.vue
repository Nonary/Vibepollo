<script setup lang="ts">
import { ref, onMounted, computed, h } from 'vue';
import { useI18n } from 'vue-i18n';
import { NDropdown, NButton } from 'naive-ui';
import {
  loadAutoTheme,
  setupThemeToggleListener,
  getPreferredTheme,
  setStoredTheme,
  setTheme,
} from '@/theme';

const { t } = useI18n();

const open = ref(false);
const current = ref('auto');

const options = computed(() => [
  {
    key: 'light',
    label: t('navbar.theme_light'),
    icon: () => h('i', { class: 'fa-solid fa-sun' }),
  },
  { key: 'dark', label: t('navbar.theme_dark'), icon: () => h('i', { class: 'fa-solid fa-moon' }) },
  {
    key: 'auto',
    label: t('navbar.theme_auto'),
    icon: () => h('i', { class: 'fa-solid fa-circle-half-stroke' }),
  },
]);

const activeIcon = computed(() => {
  const m = {
    light: 'fa-solid fa-sun',
    dark: 'fa-solid fa-moon',
    auto: 'fa-solid fa-circle-half-stroke',
  } as Record<string, string>;
  return m[current.value] || m.auto;
});

type ThemeKey = 'light' | 'dark' | 'auto';

interface ThemeOption {
  key: ThemeKey;
  label: string;
  icon: () => ReturnType<typeof h>;
}

function onSelect(key: string | number): void {
  const v = String(key) as ThemeKey;
  setStoredTheme(v);
  setTheme(v);
  current.value = v;
  open.value = false;
}

onMounted(() => {
  loadAutoTheme();
  setupThemeToggleListener();
  current.value = getPreferredTheme();
});
</script>

<template>
  <n-dropdown trigger="click" :options="options" @select="onSelect">
    <n-button tertiary size="small" class="flex items-center gap-2">
      <span class="theme-icon-active"><i :class="activeIcon" /></span>
      <span>{{ $t('navbar.toggle_theme') }}</span>
    </n-button>
  </n-dropdown>
</template>

<style scoped></style>
