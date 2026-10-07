<template>
  <div class="chart-container">
    <div class="chart-header">
      <span class="chart-title">
        <i :class="[icon, 'mr-1']" />{{ title }}
        <n-tooltip trigger="hover" :delay="300" :style="{ maxWidth: '320px' }">
          <template #trigger>
            <i class="fas fa-circle-info chart-title-tip" />
          </template>
          {{ tip }}
        </n-tooltip>
      </span>
      <span class="chart-actions">
        <span class="chart-subtitle">{{ subtitle }}</span>
        <button
          type="button"
          class="chart-expand-btn"
          :title="expandTitle"
          @click="$emit('expand')"
        >
          <i class="fas fa-expand" />
        </button>
      </span>
    </div>
    <div class="chart-wrapper">
      <slot />
    </div>
  </div>
</template>

<script setup lang="ts">
import { NTooltip } from 'naive-ui';

defineProps<{
  title: string;
  tip: string;
  subtitle: string;
  icon: string;
  expandTitle: string;
}>();

defineEmits<{
  expand: [];
}>();
</script>

<style scoped>
.chart-container {
  @apply rounded-xl border border-dark/[0.06] bg-light/[0.03] p-3 dark:border-light/[0.10] dark:bg-dark/[0.06];
}
.chart-header {
  @apply flex items-center justify-between mb-2;
}
.chart-title {
  @apply text-xs font-semibold opacity-80;
}
.chart-title-tip {
  @apply ml-1 text-[10px] opacity-50 cursor-help;
}
.chart-subtitle {
  @apply text-[10px] uppercase tracking-wider opacity-50 font-semibold;
}
.chart-wrapper {
  height: 160px;
}
.chart-actions {
  @apply flex items-center gap-2;
}
.chart-expand-btn {
  @apply text-[11px] opacity-50 hover:opacity-100 transition-opacity px-1 py-0.5 rounded;
  background: transparent;
  border: none;
  cursor: pointer;
  color: inherit;
}
.chart-expand-btn:hover {
  @apply bg-light/10 dark:bg-dark/20;
}
</style>
