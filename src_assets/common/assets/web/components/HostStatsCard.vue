<template>
  <n-card :segmented="{ content: true }" class="host-stats-card">
    <template #header>
      <div class="flex items-center gap-3">
        <i class="fas fa-microchip text-primary" />
        <span class="text-base font-semibold tracking-tight">{{ $t('host.title') }}</span>
        <n-tag v-if="!polling" size="small" round>
          {{ $t('host.idle') }}
        </n-tag>
      </div>
    </template>

    <div class="space-y-4">
      <!-- Models -->
      <div
        v-if="info.cpu_model || info.gpu_model"
        class="grid grid-cols-1 md:grid-cols-2 gap-3 text-xs opacity-80"
      >
        <div v-if="info.cpu_model" class="flex items-center gap-2">
          <i class="fas fa-server text-primary" />
          <span class="truncate" :title="info.cpu_model">
            {{ info.cpu_model }}
            <span v-if="info.cpu_logical_cores > 0" class="opacity-70">
              ({{ info.cpu_logical_cores }} {{ $t('host.cores') }})
            </span>
          </span>
        </div>
        <div v-if="info.gpu_model" class="flex items-center gap-2">
          <i class="fas fa-display text-primary" />
          <span class="truncate" :title="info.gpu_model">{{ info.gpu_model }}</span>
        </div>
      </div>

      <!-- Gauges -->
      <div class="grid grid-cols-2 md:grid-cols-4 gap-3">
        <n-tooltip trigger="hover" placement="top">
          <template #trigger>
            <div class="gauge-cell">
              <n-progress
                type="circle"
                :percentage="clampPercent(snapshot.cpu_percent)"
                :stroke-width="10"
                :show-indicator="true"
                :color="usageColor(snapshot.cpu_percent)"
              >
                <template #default>
                  <span class="gauge-value">{{ formatPercent(snapshot.cpu_percent) }}</span>
                </template>
              </n-progress>
              <div class="gauge-label">
                {{ $t('host.cpu') }}
                <span v-if="snapshot.cpu_temp_c >= 0" class="temp-pill">
                  {{ snapshot.cpu_temp_c.toFixed(0) }}°C
                </span>
              </div>
            </div>
          </template>
          {{ $t('host.tip_cpu') }}
        </n-tooltip>

        <n-tooltip trigger="hover" placement="top">
          <template #trigger>
            <div class="gauge-cell">
              <n-progress
                type="circle"
                :percentage="clampPercent(snapshot.gpu_percent)"
                :stroke-width="10"
                :show-indicator="true"
                :color="usageColor(snapshot.gpu_percent)"
              >
                <template #default>
                  <span class="gauge-value">{{ formatPercent(snapshot.gpu_percent) }}</span>
                </template>
              </n-progress>
              <div class="gauge-label">
                {{ $t('host.gpu') }}
                <span v-if="snapshot.gpu_temp_c >= 0" class="temp-pill">
                  {{ snapshot.gpu_temp_c.toFixed(0) }}°C
                </span>
              </div>
              <div v-if="snapshot.gpu_encoder_percent >= 0" class="gauge-sub">
                {{ $t('host.encoder') }}: {{ formatPercent(snapshot.gpu_encoder_percent) }}
              </div>
            </div>
          </template>
          {{ $t('host.tip_gpu') }}
        </n-tooltip>

        <n-tooltip trigger="hover" placement="top">
          <template #trigger>
            <div class="gauge-cell">
              <n-progress
                type="circle"
                :percentage="clampPercent(snapshot.ram_percent)"
                :stroke-width="10"
                :show-indicator="true"
                :color="memoryColor(snapshot.ram_percent)"
              >
                <template #default>
                  <span class="gauge-value">{{ formatPercent(snapshot.ram_percent) }}</span>
                </template>
              </n-progress>
              <div class="gauge-label">{{ $t('host.ram') }}</div>
              <div v-if="snapshot.ram_total_bytes > 0" class="gauge-sub">
                {{ formatGB(snapshot.ram_used_bytes) }} /
                {{ formatGB(snapshot.ram_total_bytes) }} GB
              </div>
            </div>
          </template>
          {{ $t('host.tip_ram') }}
        </n-tooltip>

        <n-tooltip trigger="hover" placement="top">
          <template #trigger>
            <div class="gauge-cell">
              <n-progress
                type="circle"
                :percentage="clampPercent(snapshot.vram_percent)"
                :stroke-width="10"
                :show-indicator="true"
                :color="memoryColor(snapshot.vram_percent)"
              >
                <template #default>
                  <span class="gauge-value">{{ formatPercent(snapshot.vram_percent) }}</span>
                </template>
              </n-progress>
              <div class="gauge-label">{{ $t('host.vram') }}</div>
              <div v-if="snapshot.vram_total_bytes > 0" class="gauge-sub">
                {{ formatGB(snapshot.vram_used_bytes) }} /
                {{ formatGB(snapshot.vram_total_bytes) }} GB
              </div>
            </div>
          </template>
          {{ $t('host.tip_vram') }}
        </n-tooltip>
      </div>

      <p v-if="hasNoData" class="text-xs opacity-60 italic">
        {{ $t('host.unavailable') }}
      </p>
    </div>
  </n-card>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted } from 'vue';
import { storeToRefs } from 'pinia';
import { NCard, NProgress, NTag, NTooltip } from 'naive-ui';
import { useHostStatsStore } from '@/stores/hostStats';

const store = useHostStatsStore();
const { snapshot, info, polling } = storeToRefs(store);

onMounted(() => {
  store.start();
});

onBeforeUnmount(() => {
  store.stop();
});

const hasNoData = computed(
  () =>
    snapshot.value.cpu_percent < 0 &&
    snapshot.value.gpu_percent < 0 &&
    snapshot.value.ram_total_bytes === 0,
);

const clampPercent = (v: number): number => {
  if (!Number.isFinite(v) || v < 0) {
    return 0;
  }
  return Math.min(100, Math.max(0, v));
};

const formatPercent = (v: number): string => {
  if (!Number.isFinite(v) || v < 0) {
    return 'N/A';
  }
  return `${v.toFixed(0)}%`;
};

const formatGB = (bytes: number): string => {
  if (!bytes) {
    return '0';
  }
  return (bytes / (1024 * 1024 * 1024)).toFixed(1);
};

const usageColor = (v: number): string => {
  if (v < 0) {
    return '#9ca3af';
  }
  if (v >= 90) {
    return '#ef4444';
  }
  if (v >= 70) {
    return '#f59e0b';
  }
  return '#22c55e';
};

const memoryColor = (v: number): string => {
  if (v < 0) {
    return '#9ca3af';
  }
  if (v >= 90) {
    return '#ef4444';
  }
  if (v >= 80) {
    return '#f59e0b';
  }
  return '#3b82f6';
};
</script>

<style scoped>
.gauge-cell {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 0.4rem;
  padding: 0.5rem 0.25rem;
  border-radius: 0.6rem;
}

.gauge-value {
  font-size: 0.95rem;
  font-weight: 600;
}

.gauge-label {
  font-size: 0.85rem;
  font-weight: 600;
  display: flex;
  align-items: center;
  gap: 0.4rem;
}

.gauge-sub {
  font-size: 0.7rem;
  opacity: 0.75;
}

.temp-pill {
  font-size: 0.7rem;
  padding: 0.05rem 0.4rem;
  border-radius: 999px;
  background: rgba(239, 68, 68, 0.12);
  color: rgb(239, 68, 68);
  font-weight: 600;
}
</style>
