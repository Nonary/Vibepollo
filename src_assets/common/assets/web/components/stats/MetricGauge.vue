<script setup lang="ts">
import { computed } from 'vue';

const props = withDefaults(
  defineProps<{
    label: string;
    value: number;
    detail?: string;
    color?: string;
  }>(),
  {
    detail: '',
    color: 'var(--vs-color-accent-default)',
  },
);

const bounded = computed(() =>
  Math.max(0, Math.min(100, Number.isFinite(props.value) ? props.value : 0)),
);
const display = computed(() => `${Math.round(bounded.value)}%`);
</script>

<template>
  <article
    class="metric-gauge"
    :style="{ '--gauge-color': color, '--gauge-value': `${bounded * 3.6}deg` }"
  >
    <div class="metric-gauge__ring" role="img" :aria-label="`${label}: ${display}`">
      <div>
        <strong>{{ display }}</strong
        ><span>{{ label }}</span>
      </div>
    </div>
    <p v-if="detail">{{ detail }}</p>
  </article>
</template>

<style scoped>
.metric-gauge {
  display: grid;
  min-width: 0;
  justify-items: center;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}

.metric-gauge__ring {
  display: grid;
  width: 8rem;
  height: 8rem;
  place-items: center;
  border-radius: 50%;
  background: conic-gradient(var(--gauge-color) var(--gauge-value), var(--vs-color-bg-subtle) 0);
  box-shadow: inset 0 0 0 1px color-mix(in srgb, var(--gauge-color) 25%, transparent);
}

.metric-gauge__ring::before {
  width: 6.3rem;
  height: 6.3rem;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: 50%;
  background: var(--vs-color-bg-surface);
  content: '';
  grid-area: 1 / 1;
}

.metric-gauge__ring > div {
  display: grid;
  max-width: 5.5rem;
  gap: var(--vs-space-2);
  grid-area: 1 / 1;
  text-align: center;
  z-index: 1;
}

.metric-gauge strong {
  font-size: 1.55rem;
  font-variant-numeric: tabular-nums;
  line-height: 1;
}

.metric-gauge span,
.metric-gauge p {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.metric-gauge p {
  min-height: 1rem;
  overflow: hidden;
  text-align: center;
  text-overflow: ellipsis;
  white-space: nowrap;
}
</style>
