<template>
  <div class="stats-page space-y-5 px-2 pb-10 md:px-4">
    <section class="stats-header">
      <div class="min-w-0">
        <h1 class="text-xl md:text-2xl font-semibold text-dark dark:text-light">
          {{ $t('stats.title') }}
        </h1>
        <p class="stats-header__subtitle">{{ $t('stats.subtitle') }}</p>
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

    <div class="stats-flow">
      <template v-if="statsEnabled">
        <ActiveSessionsCard />

        <div class="stats-grid">
          <HostStatsCard
            :active="statsEnabled"
            :poll-interval-ms="pollIntervalMs"
            :history-retention-seconds="historyRetentionSeconds"
            :max-history-points="maxHistoryPoints"
          />
          <HostStatsHistoryCard />
        </div>
      </template>

      <SessionHistoryCard />
    </div>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { storeToRefs } from 'pinia';
import { NAlert, NButton } from 'naive-ui';
import ActiveSessionsCard from '@/components/ActiveSessionsCard.vue';
import HostStatsCard from '@/components/HostStatsCard.vue';
import HostStatsHistoryCard from '@/components/HostStatsHistoryCard.vue';
import SessionHistoryCard from '@/components/SessionHistoryCard.vue';
import { useAuthStore } from '@/stores/auth';
import { useConfigStore } from '@/stores/config';

const auth = useAuthStore();
const configStore = useConfigStore();
const { config } = storeToRefs(configStore);
const savingStatsPreference = ref(false);

const statsEnabled = computed(() => Boolean(config.value?.realtime_stats_enabled));
const pollIntervalMs = computed(
  () => Number(config.value?.realtime_stats_poll_interval_ms) || 2000,
);
const historyRetentionSeconds = computed(
  () => Number(config.value?.realtime_stats_history_retention_seconds) || 300,
);
const maxHistoryPoints = computed(
  () => Number(config.value?.realtime_stats_max_history_points) || 300,
);

async function enableStats() {
  savingStatsPreference.value = true;
  try {
    configStore.updateOption('realtime_stats_enabled', true);
    configStore.updateOption('realtime_stats_dashboard_prompt_dismissed', true);
    await configStore.flushPatchQueue();
  } finally {
    savingStatsPreference.value = false;
  }
}

onMounted(async () => {
  await auth.waitForAuthentication();
  await configStore.fetchConfig();
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
}

.stats-header__subtitle {
  margin-top: 4px;
  font-size: 13px;
  line-height: 1.45;
  opacity: 0.72;
}

.stats-flow {
  display: grid;
  gap: 18px;
}

.stats-grid {
  display: grid;
  gap: 18px;
}

@media (min-width: 1280px) {
  .stats-grid {
    grid-template-columns: minmax(0, 1fr) minmax(360px, 0.9fr);
    align-items: start;
  }
}

@media (max-width: 640px) {
  .stats-header {
    align-items: flex-start;
    flex-direction: column;
  }
}
</style>
