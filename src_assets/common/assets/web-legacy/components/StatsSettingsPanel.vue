<script setup lang="ts">
import { storeToRefs } from 'pinia';
import ConfigFieldRenderer from '@/ConfigFieldRenderer.vue';
import { useConfigStore } from '@/stores/config';

const store = useConfigStore();
const { config } = storeToRefs(store);
</script>

<template>
  <div class="stats-settings space-y-6">
    <section class="space-y-4">
      <h4 class="stats-settings__heading">{{ $t('stats.section_collection') }}</h4>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_enabled"
          setting-key="realtime_stats_enabled"
        />
      </div>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_poll_interval_ms"
          setting-key="realtime_stats_poll_interval_ms"
          :disabled="!config.realtime_stats_enabled"
        />
      </div>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_pause_when_hidden"
          setting-key="realtime_stats_pause_when_hidden"
        />
      </div>
    </section>

    <section class="space-y-4">
      <h4 class="stats-settings__heading">{{ $t('stats.section_charts') }}</h4>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_history_retention_seconds"
          setting-key="realtime_stats_history_retention_seconds"
        />
      </div>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_max_history_points"
          setting-key="realtime_stats_max_history_points"
        />
      </div>
    </section>

    <section class="space-y-3">
      <h4 class="stats-settings__heading">{{ $t('stats.section_cards') }}</h4>
      <p class="text-xs opacity-70 m-0">{{ $t('stats.section_cards_desc') }}</p>
      <div class="grid gap-3 sm:grid-cols-2">
        <ConfigFieldRenderer
          v-model="config.realtime_stats_show_active_sessions"
          setting-key="realtime_stats_show_active_sessions"
        />
        <ConfigFieldRenderer
          v-model="config.realtime_stats_show_host_stats"
          setting-key="realtime_stats_show_host_stats"
        />
        <ConfigFieldRenderer
          v-model="config.realtime_stats_show_host_charts"
          setting-key="realtime_stats_show_host_charts"
        />
        <ConfigFieldRenderer
          v-model="config.realtime_stats_show_session_history"
          setting-key="realtime_stats_show_session_history"
        />
      </div>
    </section>

    <section class="space-y-4">
      <h4 class="stats-settings__heading">{{ $t('stats.section_history') }}</h4>
      <p class="text-xs opacity-70 m-0">{{ $t('stats.section_history_desc') }}</p>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.session_history_enabled"
          setting-key="session_history_enabled"
        />
      </div>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.session_history_ttl_days"
          setting-key="session_history_ttl_days"
        />
      </div>
      <div class="stats-settings__field">
        <ConfigFieldRenderer
          v-model="config.session_history_db_size_limit_mb"
          setting-key="session_history_db_size_limit_mb"
        />
      </div>
    </section>
  </div>
</template>

<style scoped>
.stats-settings__heading {
  margin: 0;
  font-size: 12px;
  font-weight: 600;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  opacity: 0.65;
}

.stats-settings__field {
  min-width: 0;
}

.stats-settings__field :deep(.n-input),
.stats-settings__field :deep(.n-input-number) {
  width: 100%;
  max-width: 24rem;
}

@media (max-width: 640px) {
  .stats-settings__field :deep(.n-input),
  .stats-settings__field :deep(.n-input-number) {
    max-width: none;
  }
}
</style>
