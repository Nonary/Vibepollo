<template>
  <n-modal
    :show="show"
    preset="card"
    :title="title"
    style="width: min(95vw, 1100px)"
    :bordered="false"
    size="huge"
    :segmented="{ content: true }"
    @update:show="$emit('update:show', $event)"
    @after-leave="$emit('after-leave')"
  >
    <template #header-extra>
      <div class="flex items-center gap-1">
        <button
          type="button"
          class="chart-expand-btn"
          :title="zoomOutTitle"
          @click="$emit('zoom-out')"
        >
          <i class="fas fa-minus" />
        </button>
        <button
          type="button"
          class="chart-expand-btn"
          :title="zoomResetTitle"
          @click="$emit('zoom-reset')"
        >
          <i class="fas fa-rotate-left" />
        </button>
        <button
          type="button"
          class="chart-expand-btn"
          :title="zoomInTitle"
          @click="$emit('zoom-in')"
        >
          <i class="fas fa-plus" />
        </button>
      </div>
    </template>
    <div class="chart-wrapper-zoom">
      <slot />
    </div>
    <div class="zoom-hint">
      <i class="fas fa-circle-info" />
      {{ hint }}
    </div>
  </n-modal>
</template>

<script setup lang="ts">
import { NModal } from 'naive-ui';

defineProps<{
  show: boolean;
  title: string;
  hint: string;
  zoomInTitle: string;
  zoomOutTitle: string;
  zoomResetTitle: string;
}>();

defineEmits<{
  'update:show': [value: boolean];
  'zoom-in': [];
  'zoom-out': [];
  'zoom-reset': [];
  'after-leave': [];
}>();
</script>

<style scoped>
.chart-wrapper-zoom {
  height: 60vh;
  min-height: 360px;
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
.zoom-hint {
  @apply mt-2 text-[11px] opacity-60 text-center flex items-center justify-center gap-2;
}
</style>
