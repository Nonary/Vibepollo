<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';

import { hostHistoryPeaks, type HostHistoryPoint } from '@/utils/v2Parity';

const props = defineProps<{
  title: string;
  points: HostHistoryPoint[];
  current: { cpu: number | null; gpu: number | null; encoder: number | null };
}>();

const { t } = useI18n();
const width = 320;
const height = 124;
const padding = 8;
const series = [
  { key: 'cpu', label: 'CPU', color: 'var(--vs-color-status-info)' },
  { key: 'gpu', label: 'GPU', color: 'var(--vs-color-status-success)' },
  { key: 'encoder', label: 'GPU encoder', color: 'var(--vs-color-data-accent)' },
] as const;

const peak = computed(() => hostHistoryPeaks(props.points));
const seriesPoints = computed(() =>
  series.map((entry) => {
    const values = props.points.map((point) =>
      Number(point[`${entry.key}_percent` as keyof HostHistoryPoint]),
    );
    const finite = values.map((value) => (Number.isFinite(value) ? value : 0));
    const points = finite.map((value, index) => {
      const x =
        padding +
        (finite.length <= 1
          ? width - padding * 2
          : (index / (finite.length - 1)) * (width - padding * 2));
      const y =
        height - padding - (Math.max(0, Math.min(100, value)) / 100) * (height - padding * 2);
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    });
    return { ...entry, points: points.join(' ') };
  }),
);

function value(value: number | null | undefined): string {
  return value == null || !Number.isFinite(value) ? '--' : `${Math.round(value)}%`;
}
</script>

<template>
  <article class="host-compute-chart">
    <header class="host-compute-chart__header">
      <h4>{{ title }}</h4>
      <div class="host-compute-chart__legend" aria-label="Compute history legend">
        <span v-for="entry in series" :key="entry.key" class="host-compute-chart__legend-item">
          <i :style="{ background: entry.color }" aria-hidden="true" />{{ entry.label }}
        </span>
      </div>
    </header>
    <svg
      viewBox="0 0 320 124"
      preserveAspectRatio="none"
      role="img"
      :aria-label="title"
      class="host-compute-chart__plot"
    >
      <line
        v-for="grid in [32, 62, 92]"
        :key="grid"
        x1="0"
        :y1="grid"
        x2="320"
        :y2="grid"
        class="host-compute-chart__grid"
      />
      <text x="4" y="14" class="host-compute-chart__axis">100%</text>
      <text x="4" y="120" class="host-compute-chart__axis">0%</text>
      <polyline
        v-for="entry in seriesPoints"
        :key="entry.key"
        :points="entry.points"
        fill="none"
        :stroke="entry.color"
        class="host-compute-chart__line"
      />
    </svg>
    <footer class="host-compute-chart__footer">
      <span
        >CPU · {{ t('stats.current') }} {{ value(current.cpu) }} · {{ t('stats.peak') }}
        {{ value(peak.cpu) }}</span
      >
      <span
        >GPU · {{ t('stats.current') }} {{ value(current.gpu) }} · {{ t('stats.peak') }}
        {{ value(peak.gpu) }}</span
      >
      <span
        >ENC · {{ t('stats.current') }} {{ value(current.encoder) }} · {{ t('stats.peak') }}
        {{ value(peak.encoder) }}</span
      >
    </footer>
  </article>
</template>

<style scoped>
.host-compute-chart {
  overflow: hidden;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
}
.host-compute-chart__header {
  display: grid;
  gap: var(--vs-space-8);
  padding: var(--vs-space-16) var(--vs-space-16) var(--vs-space-8);
}
.host-compute-chart h4 {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}
.host-compute-chart__legend {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
.host-compute-chart__legend-item {
  display: inline-flex;
  align-items: center;
  gap: var(--vs-space-4);
}
.host-compute-chart__legend-item i {
  width: 0.55rem;
  height: 0.55rem;
  border-radius: 50%;
}
.host-compute-chart__plot {
  display: block;
  width: 100%;
  height: 9rem;
  padding: 0 var(--vs-space-12);
}
.host-compute-chart__grid {
  stroke: var(--vs-color-border-subtle);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
}
.host-compute-chart__axis {
  fill: var(--vs-color-text-muted);
  font-size: 8px;
}
.host-compute-chart__line {
  stroke-width: 2;
  stroke-linecap: round;
  stroke-linejoin: round;
  vector-effect: non-scaling-stroke;
}
.host-compute-chart__footer {
  display: flex;
  flex-wrap: wrap;
  justify-content: space-between;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-16) 0;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
}
.host-compute-chart small {
  display: block;
  padding: var(--vs-space-4) var(--vs-space-16) var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
</style>
