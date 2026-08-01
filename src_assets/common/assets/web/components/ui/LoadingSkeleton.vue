<script setup lang="ts">
// Geometry is configurable so placeholders can match their final content.
import { computed, type CSSProperties } from 'vue';
import { useI18n } from 'vue-i18n';

const { t } = useI18n();

type SkeletonVariant = 'text' | 'block' | 'circle';

const props = withDefaults(
  defineProps<{
    variant?: SkeletonVariant;
    lines?: number;
    width?: string;
    height?: string;
    label?: string;
  }>(),
  {
    variant: 'text',
    lines: 1,
    width: '100%',
  },
);

const normalizedLines = computed(() =>
  props.variant === 'text' ? Math.max(1, Math.min(12, Math.floor(props.lines))) : 1,
);

function skeletonStyle(line: number): CSSProperties {
  const isLastTextLine = props.variant === 'text' && normalizedLines.value > 1 && line === normalizedLines.value;
  return {
    '--vs-skeleton-width': isLastTextLine ? '72%' : props.width,
    '--vs-skeleton-height': props.height,
  } as CSSProperties;
}
</script>

<template>
  <div
    class="vs-loading-skeleton"
    :class="'vs-loading-skeleton--' + variant"
    role="status"
    aria-live="polite"
    aria-busy="true"
  >
    <span class="vs-sr-only">{{ label || t('_common.loading') }}</span>
    <span
      v-for="line in normalizedLines"
      :key="line"
      class="vs-loading-skeleton__shape"
      :style="skeletonStyle(line)"
      aria-hidden="true"
    />
  </div>
</template>
