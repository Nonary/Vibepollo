<script setup lang="ts">
import StatsSettingsPanel from '@/components/StatsSettingsPanel.vue';
</script>

<template>
  <div id="stats" class="config-page space-y-5">
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
          <router-link
            to="/stats"
            class="mt-2 inline-flex items-center gap-1.5 text-xs font-medium text-primary hover:underline"
          >
            <i class="fas fa-arrow-up-right-from-square text-[10px]" />
            <span>{{ $t('stats.open_stats_page') }}</span>
          </router-link>
        </div>
      </div>
    </div>

    <StatsSettingsPanel />
  </div>
</template>
