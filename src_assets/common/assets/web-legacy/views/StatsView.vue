<template>
  <div class="stats-page space-y-5 pb-10">
    <section class="stats-header">
      <div class="min-w-0">
        <h1 class="text-xl md:text-2xl font-semibold text-dark dark:text-light">
          {{ $t('stats.title') }}
        </h1>
        <p class="stats-header__subtitle">{{ $t('stats.subtitle') }}</p>
      </div>
      <div class="stats-header__actions">
        <n-tag
          v-if="statsEnabled"
          :type="hostStats.polling ? 'success' : 'default'"
          size="small"
          round
        >
          <span class="inline-flex items-center gap-1.5">
            <span class="stats-live-dot" :class="{ 'stats-live-dot--active': hostStats.polling }" />
            {{ hostStats.polling ? $t('stats.live') : $t('stats.paused') }}
          </span>
        </n-tag>
        <span v-if="statsEnabled && lastUpdatedLabel" class="stats-header__updated">
          {{ lastUpdatedLabel }}
        </span>
        <n-button
          v-if="statsEnabled"
          size="small"
          secondary
          :loading="refreshing"
          @click="refreshNow"
        >
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ $t('_common.refresh') }}</span>
        </n-button>
        <n-button
          size="small"
          :type="showSettings ? 'primary' : 'default'"
          secondary
          @click="showSettings = !showSettings"
        >
          <i class="fas fa-sliders" />
          <span class="ml-2">{{ $t('stats.settings') }}</span>
        </n-button>
      </div>
    </section>

    <n-alert v-if="!statsEnabled" type="info" :show-icon="true" class="rounded-lg">
      <div class="flex flex-col gap-3 md:flex-row md:items-center md:justify-between">
        <div class="min-w-0">
          <p class="m-0 text-sm font-medium">{{ $t('stats.disabled_title') }}</p>
          <p class="m-0 mt-1 text-xs opacity-75">{{ $t('stats.disabled_desc') }}</p>
        </div>
        <n-button
          size="small"
          type="primary"
          strong
          class="w-full justify-center md:w-auto md:shrink-0"
          :loading="savingStatsPreference"
          @click="enableStats"
        >
          <i class="fas fa-chart-line" />
          <span>{{ $t('stats.enable') }}</span>
        </n-button>
      </div>
    </n-alert>

    <n-collapse-transition :show="showSettings">
      <n-card class="stats-settings-card" :segmented="{ content: true, footer: false }">
        <template #header>
          <div class="flex items-center gap-3">
            <span
              class="inline-flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary"
            >
              <i class="fas fa-sliders text-sm" />
            </span>
            <div class="min-w-0">
              <h2 class="text-base font-medium m-0">{{ $t('stats.config_header') }}</h2>
              <p class="text-xs opacity-70 m-0 mt-0.5">{{ $t('stats.config_desc') }}</p>
            </div>
          </div>
        </template>
        <StatsSettingsPanel />
      </n-card>
    </n-collapse-transition>

    <div class="stats-flow">
      <template v-if="statsEnabled">
        <ActiveSessionsCard v-if="showActiveSessions" />

        <div
          v-if="showHostStats || showHostCharts"
          class="stats-grid"
          :class="{ 'stats-grid--single': !(showHostStats && showHostCharts) }"
        >
          <HostStatsCard v-if="showHostStats" />
          <HostStatsHistoryCard v-if="showHostCharts" />
        </div>
      </template>

      <SessionHistoryCard v-if="showSessionHistory" />

      <div v-if="statsEnabled && nothingVisible" class="stats-empty">
        <i class="fas fa-eye-slash" />
        <span>{{ $t('stats.all_cards_hidden') }}</span>
        <n-button size="small" tertiary @click="showSettings = true">
          {{ $t('stats.settings') }}
        </n-button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { storeToRefs } from 'pinia';
import { useI18n } from 'vue-i18n';
import { NAlert, NButton, NCard, NCollapseTransition, NTag } from 'naive-ui';
import ActiveSessionsCard from '@/components/ActiveSessionsCard.vue';
import HostStatsCard from '@/components/HostStatsCard.vue';
import HostStatsHistoryCard from '@/components/HostStatsHistoryCard.vue';
import SessionHistoryCard from '@/components/SessionHistoryCard.vue';
import StatsSettingsPanel from '@/components/StatsSettingsPanel.vue';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';
import { useHostStatsStore } from '@/stores/hostStats';

const { t } = useI18n();
const auth = useAuthStore();
const configStore = useConfigStore();
const hostStats = useHostStatsStore();
const { config } = storeToRefs(configStore);
const savingStatsPreference = ref(false);
const showSettings = ref(false);
const refreshing = ref(false);
const nowTick = ref(Date.now());
let tickTimer = 0;

const statsEnabled = computed(() => Boolean(config.value?.realtime_stats_enabled ?? true));
const showActiveSessions = computed(() =>
  Boolean(config.value?.realtime_stats_show_active_sessions ?? true),
);
const showHostStats = computed(() => Boolean(config.value?.realtime_stats_show_host_stats ?? true));
const showHostCharts = computed(() =>
  Boolean(config.value?.realtime_stats_show_host_charts ?? true),
);
const showSessionHistory = computed(() =>
  Boolean(config.value?.realtime_stats_show_session_history ?? true),
);
const nothingVisible = computed(
  () =>
    !showActiveSessions.value &&
    !showHostStats.value &&
    !showHostCharts.value &&
    !showSessionHistory.value,
);

const lastUpdatedLabel = computed(() => {
  const ts = hostStats.lastUpdated;
  if (!ts) return '';
  const seconds = Math.max(0, Math.round((nowTick.value - ts) / 1000));
  return seconds <= 1 ? t('stats.updated_just_now') : t('stats.updated_seconds_ago', { seconds });
});

async function enableStats() {
  savingStatsPreference.value = true;
  try {
    configStore.updateOption('realtime_stats_enabled', true);
    await configStore.flushPatchQueue();
  } finally {
    savingStatsPreference.value = false;
  }
}

async function refreshNow() {
  refreshing.value = true;
  try {
    await hostStats.refresh();
  } finally {
    refreshing.value = false;
  }
}

onMounted(async () => {
  tickTimer = window.setInterval(() => {
    nowTick.value = Date.now();
  }, 1000);
  await auth.waitForAuthentication();
  await configStore.fetchConfig();
});

onUnmounted(() => {
  if (tickTimer) window.clearInterval(tickTimer);
});
</script>

<style scoped>
.stats-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  border: 1px solid rgba(148, 163, 184, 0.24);
  border-radius: 8px;
  padding: 16px;
  background: rgba(148, 163, 184, 0.08);
  flex-wrap: wrap;
}

.stats-header__subtitle {
  margin-top: 4px;
  font-size: 13px;
  line-height: 1.45;
  opacity: 0.72;
}

.stats-header__actions {
  display: flex;
  align-items: center;
  gap: 10px;
  flex-wrap: wrap;
}

.stats-header__updated {
  font-size: 11px;
  opacity: 0.6;
  white-space: nowrap;
}

.stats-live-dot {
  width: 7px;
  height: 7px;
  border-radius: 9999px;
  background: rgba(148, 163, 184, 0.7);
}

.stats-live-dot--active {
  background: #22c55e;
  animation: stats-pulse 2s ease-in-out infinite;
}

@keyframes stats-pulse {
  0%,
  100% {
    opacity: 1;
  }
  50% {
    opacity: 0.35;
  }
}

.stats-flow {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 18px;
}

.stats-grid {
  display: grid;
  grid-template-columns: minmax(0, 1fr);
  gap: 18px;
}

.stats-flow > *,
.stats-grid > * {
  min-width: 0;
}

@media (min-width: 1280px) {
  .stats-grid:not(.stats-grid--single) {
    grid-template-columns: minmax(0, 1fr) minmax(360px, 0.9fr);
    align-items: start;
  }
}

.stats-empty {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 8px;
  padding: 40px 16px;
  border: 1px dashed rgba(148, 163, 184, 0.35);
  border-radius: 12px;
  font-size: 13px;
  opacity: 0.75;
}

@media (max-width: 640px) {
  .stats-header {
    align-items: flex-start;
    flex-direction: column;
  }
}
</style>
