<template>
  <n-tooltip v-if="tip" trigger="hover" placement="top" :delay="300" :style="{ maxWidth: '320px' }">
    <template #trigger>
      <div class="stat-cell" :class="{ 'stat-cell-help': true }">
        <div class="stat-label">
          {{ label }}
          <i class="fas fa-circle-info stat-help-icon" />
        </div>
        <div class="stat-value">
          <slot>{{ value }}</slot>
        </div>
        <div v-if="subValue" class="stat-subvalue">{{ subValue }}</div>
      </div>
    </template>
    <span>{{ tip }}</span>
  </n-tooltip>
  <div v-else class="stat-cell">
    <div class="stat-label">{{ label }}</div>
    <div class="stat-value">
      <slot>{{ value }}</slot>
    </div>
    <div v-if="subValue" class="stat-subvalue">{{ subValue }}</div>
  </div>
</template>

<script setup lang="ts">
import { NTooltip } from 'naive-ui';

defineProps<{
  label: string;
  value?: string | number;
  tip?: string;
  subValue?: string;
}>();
</script>

<style scoped>
.stat-cell {
  @apply rounded-lg bg-dark/[0.04] dark:bg-light/[0.06] px-3 py-2;
}
.stat-cell-help {
  @apply cursor-help;
}
.stat-label {
  @apply text-[10px] uppercase tracking-wider opacity-60 font-semibold mb-0.5 flex items-center gap-1;
}
.stat-help-icon {
  @apply text-[9px] opacity-50;
}
.stat-value {
  @apply text-sm font-mono font-semibold;
}
.stat-subvalue {
  @apply text-[10px] font-mono opacity-60 mt-0.5;
}
</style>
