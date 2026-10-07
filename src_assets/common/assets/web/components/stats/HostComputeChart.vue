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
const timeDomain = computed(() => {
  const timestamps = props.points
    .map((point) => point.timestamp)
    .filter((timestamp) => Number.isFinite(timestamp));
  const minimum = timestamps.length ? Math.min(...timestamps) : 0;
  const maximum = timestamps.length ? Math.max(...timestamps) : minimum + 1;
  return { minimum, maximum: maximum === minimum ? minimum + 1 : maximum };
});
const gapThreshold = computed(() => {
  const deltas = props.points
    .slice(1)
    .map((point, index) => point.timestamp - (props.points[index]?.timestamp ?? point.timestamp))
    .filter((delta) => delta > 0 && Number.isFinite(delta))
    .sort((left, right) => left - right);
  if (!deltas.length) return Number.POSITIVE_INFINITY;
  const median = deltas[Math.floor(deltas.length / 2)] ?? 0;
  return Math.max(15_000, median * 3);
});

function valueFor(point: HostHistoryPoint, key: (typeof series)[number]['key']): number | null {
  const raw = point[`${key}_percent` as keyof HostHistoryPoint];
  if (raw == null) return null;
  const value = Number(raw);
  return Number.isFinite(value) && value >= 0 ? Math.min(100, value) : null;
}

function xFor(timestamp: number): number {
  const span = timeDomain.value.maximum - timeDomain.value.minimum;
  return padding + ((timestamp - timeDomain.value.minimum) / span) * (width - padding * 2);
}

const seriesPoints = computed(() =>
  series.map((entry) => {
    const segments: string[] = [];
    let current: string[] = [];
    let previousTimestamp: number | undefined;
    for (const point of props.points) {
      const value = valueFor(point, entry.key);
      const gap =
        previousTimestamp != null && point.timestamp - previousTimestamp > gapThreshold.value;
      if (value == null || gap) {
        if (current.length) segments.push(current.join(' '));
        current = [];
      }
      if (value != null) {
        const x = xFor(point.timestamp);
        const y =
          height - padding - (Math.max(0, Math.min(100, value)) / 100) * (height - padding * 2);
        current.push(`${x.toFixed(2)},${y.toFixed(2)}`);
      }
      previousTimestamp = point.timestamp;
    }
    if (current.length) segments.push(current.join(' '));
    return { ...entry, segments };
  }),
);

const axisLabels = computed(() => {
  const domain = timeDomain.value;
  const compact = domain.maximum - domain.minimum < 5_000;
  return [0, 0.5, 1].map((fraction) => {
    const timestamp = domain.minimum + (domain.maximum - domain.minimum) * fraction;
    return {
      x: Math.max(22, Math.min(width - 22, xFor(timestamp))),
      label: formatTimestamp(timestamp, compact),
      show: !compact || fraction !== 0.5,
    };
  });
});

function formatTimestamp(timestamp: number, includeMilliseconds = false): string {
  if (timestamp < 100_000_000) return `#${Math.round(timestamp) + 1}`;
  const options: Intl.DateTimeFormatOptions = {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  };
  if (includeMilliseconds) options.fractionalSecondDigits = 3;
  return new Intl.DateTimeFormat(undefined, options).format(new Date(timestamp));
}

function axisLabelStyle(axis: { x: number }, index: number): Record<string, string> {
  const left = `${(axis.x / width) * 100}%`;
  if (index === 0) return { left, textAlign: 'left' };
  if (index === 2) return { left, transform: 'translateX(-100%)', textAlign: 'right' };
  return { left, transform: 'translateX(-50%)', textAlign: 'center' };
}

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
    <div class="host-compute-chart__plot">
      <div class="host-compute-chart__surface">
        <svg viewBox="0 0 320 124" preserveAspectRatio="none" role="img" :aria-label="title">
          <line
            v-for="grid in [32, 62, 92]"
            :key="grid"
            x1="0"
            :y1="grid"
            x2="320"
            :y2="grid"
            class="host-compute-chart__grid"
          />
          <template v-for="entry in seriesPoints" :key="entry.key">
            <polyline
              v-for="segment in entry.segments"
              :key="`${entry.key}:${segment}`"
              :points="segment"
              fill="none"
              :stroke="entry.color"
              class="host-compute-chart__line"
            />
          </template>
        </svg>
        <span class="host-compute-chart__y-axis host-compute-chart__y-axis--top" aria-hidden="true"
          >100%</span
        >
        <span
          class="host-compute-chart__y-axis host-compute-chart__y-axis--bottom"
          aria-hidden="true"
          >0%</span
        >
      </div>
      <div class="host-compute-chart__axis-row" aria-hidden="true">
        <template v-for="(axis, index) in axisLabels" :key="`${axis.label}:${index}`">
          <span
            v-if="axis.show"
            class="host-compute-chart__axis"
            :style="axisLabelStyle(axis, index)"
            >{{ axis.label }}</span
          >
        </template>
      </div>
    </div>
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
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 10.5rem;
  padding: 0 var(--vs-space-12);
}
.host-compute-chart__surface {
  position: relative;
  flex: 1;
  min-height: 0;
}
.host-compute-chart__surface svg {
  display: block;
  width: 100%;
  height: 100%;
}
.host-compute-chart__grid {
  stroke: var(--vs-color-border-subtle);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
}
.host-compute-chart__axis {
  fill: var(--vs-color-text-muted);
  font-size: 11px;
}
.host-compute-chart__y-axis {
  position: absolute;
  left: 0;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  line-height: 1;
  pointer-events: none;
  white-space: nowrap;
}
.host-compute-chart__y-axis--top {
  top: 0.15rem;
}
.host-compute-chart__y-axis--bottom {
  bottom: 0.15rem;
}
.host-compute-chart__axis-row {
  position: relative;
  flex: none;
  height: 1.5rem;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
  line-height: 1.25rem;
  white-space: nowrap;
}
.host-compute-chart__axis-row .host-compute-chart__axis {
  position: absolute;
  top: 0;
  color: inherit;
  font-size: inherit;
  line-height: inherit;
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
