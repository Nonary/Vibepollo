<script setup lang="ts">
import { storeToRefs } from 'pinia';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { config } = storeToRefs(store);
</script>

<template>
  <div id="realtime_stats" class="config-page realtime-stats-config space-y-5">
    <div
      class="rounded-xl border border-dark/10 bg-light/60 px-4 py-3 dark:border-light/10 dark:bg-dark/40 sm:px-5 sm:py-4"
    >
      <div class="flex items-start gap-3">
        <span
          class="mt-0.5 inline-flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary"
        >
          <i class="fas fa-chart-line text-sm" />
        </span>
        <div class="min-w-0">
          <h3 class="text-sm font-semibold leading-tight">{{ $t('stats.config_header') }}</h3>
          <p class="mt-1 text-xs leading-relaxed opacity-70">
            {{ $t('stats.config_desc') }}
          </p>
        </div>
      </div>
    </div>

    <div class="space-y-5 sm:space-y-6">
      <div class="realtime-stats-setting">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_enabled"
          setting-key="realtime_stats_enabled"
        />
      </div>

      <div class="realtime-stats-setting">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_poll_interval_ms"
          setting-key="realtime_stats_poll_interval_ms"
        />
      </div>

      <div class="realtime-stats-setting">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_history_retention_seconds"
          setting-key="realtime_stats_history_retention_seconds"
        />
      </div>

      <div class="realtime-stats-setting">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_max_history_points"
          setting-key="realtime_stats_max_history_points"
        />
      </div>

    </div>
  </div>
</template>

<style scoped>
.realtime-stats-setting {
  min-width: 0;
}

.realtime-stats-setting :deep(.n-input),
.realtime-stats-setting :deep(.n-input-number) {
  width: 100%;
  max-width: 24rem;
}

@media (max-width: 640px) {
  .realtime-stats-setting :deep(.n-input),
  .realtime-stats-setting :deep(.n-input-number) {
    max-width: none;
  }
}
</style>
