<script setup lang="ts">
import { computed, useId } from 'vue';
import { useI18n } from 'vue-i18n';

const props = withDefaults(
  defineProps<{
    title: string;
    value: string;
    values: number[];
    unit?: string;
    description?: string;
    color?: string;
    ceiling?: number;
    target?: number;
  }>(),
  {
    unit: '',
    description: '',
    color: 'var(--vs-color-accent-default)',
  },
);

const chartWidth = 320;
const chartHeight = 104;
const paddingX = 4;
const paddingY = 8;
const uid = useId().replace(/:/g, '');
const gradientId = `metric-fill-${uid}`;
const { t } = useI18n();

const finiteValues = computed(() => props.values.filter((value) => Number.isFinite(value)));
const upperBound = computed(() => {
  if (props.ceiling && props.ceiling > 0) return props.ceiling;
  const largest = Math.max(...finiteValues.value, props.target ?? 0, 1);
  return largest * 1.12;
});

const points = computed(() => {
  const values = finiteValues.value;
  if (!values.length) return '';
  const width = chartWidth - paddingX * 2;
  const height = chartHeight - paddingY * 2;
  return values
    .map((value, index) => {
      const x = paddingX + (values.length === 1 ? width : (index / (values.length - 1)) * width);
      const y = paddingY + height - (Math.max(0, value) / upperBound.value) * height;
      return `${x.toFixed(2)},${Math.max(paddingY, y).toFixed(2)}`;
    })
    .join(' ');
});

const areaPath = computed(() => {
  if (!points.value) return '';
  const firstX = points.value.split(' ')[0]?.split(',')[0] ?? paddingX;
  const lastX = points.value.split(' ').at(-1)?.split(',')[0] ?? chartWidth - paddingX;
  return `M ${firstX} ${chartHeight - paddingY} L ${points.value.replaceAll(' ', ' L ')} L ${lastX} ${chartHeight - paddingY} Z`;
});

const lastPoint = computed(() => {
  const value = finiteValues.value.at(-1);
  if (value == null) return null;
  const width = chartWidth - paddingX * 2;
  const height = chartHeight - paddingY * 2;
  return {
    x: chartWidth - paddingX,
    y: Math.max(paddingY, paddingY + height - (Math.max(0, value) / upperBound.value) * height),
  };
});

const targetY = computed(() => {
  if (props.target == null || props.target < 0) return null;
  const height = chartHeight - paddingY * 2;
  return Math.max(paddingY, paddingY + height - (props.target / upperBound.value) * height);
});

const minimum = computed(() =>
  finiteValues.value.length
    ? Math.min(...finiteValues.value).toLocaleString(undefined, { maximumFractionDigits: 1 })
    : '—',
);
const maximum = computed(() =>
  finiteValues.value.length
    ? Math.max(...finiteValues.value).toLocaleString(undefined, { maximumFractionDigits: 1 })
    : '—',
);
</script>

<template>
  <article class="metric-chart" :style="{ '--metric-color': color }">
    <header class="metric-chart__header">
      <div>
        <h4 :title="description">{{ title }}</h4>
        <p v-if="description">{{ description }}</p>
      </div>
      <strong class="metric-chart__value">{{ value }}</strong>
    </header>

    <div class="metric-chart__plot">
      <svg
        viewBox="0 0 320 104"
        preserveAspectRatio="none"
        role="img"
        :aria-label="`${title}: ${value}`"
      >
        <defs>
          <linearGradient :id="gradientId" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="var(--metric-color)" stop-opacity="0.32" />
            <stop offset="1" stop-color="var(--metric-color)" stop-opacity="0.02" />
          </linearGradient>
        </defs>
        <line
          v-for="grid in [26, 52, 78]"
          :key="grid"
          x1="0"
          :y1="grid"
          x2="320"
          :y2="grid"
          class="metric-chart__grid"
        />
        <line
          v-if="targetY != null"
          x1="0"
          :y1="targetY"
          x2="320"
          :y2="targetY"
          class="metric-chart__target"
        />
        <path v-if="areaPath" :d="areaPath" :fill="`url(#${gradientId})`" />
        <polyline v-if="points" :points="points" class="metric-chart__line" />
        <circle
          v-if="lastPoint"
          :cx="lastPoint.x"
          :cy="lastPoint.y"
          r="3.5"
          class="metric-chart__last-point"
        />
        <line v-else x1="4" y1="78" x2="316" y2="78" class="metric-chart__empty-line" />
      </svg>
    </div>

    <footer class="metric-chart__footer">
      <span>{{ t('ui.stats.minimum', { value: `${minimum}${unit}` }) }}</span>
      <span>{{
        t('ui.stats.sample_count', { count: finiteValues.length }, finiteValues.length)
      }}</span>
      <span>{{ t('ui.stats.maximum', { value: `${maximum}${unit}` }) }}</span>
    </footer>
  </article>
</template>

<style scoped>
.metric-chart {
  min-width: 0;
  overflow: hidden;
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background:
    linear-gradient(
      145deg,
      color-mix(in srgb, var(--metric-color) 7%, transparent),
      transparent 58%
    ),
    var(--vs-color-bg-surface);
}

.metric-chart__header {
  display: flex;
  min-height: 5.25rem;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16) var(--vs-space-16) var(--vs-space-8);
}

.metric-chart__header > div {
  min-width: 0;
}

.metric-chart h4 {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-semibold);
}

.metric-chart__header p {
  display: -webkit-box;
  margin-top: var(--vs-space-4);
  overflow: hidden;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  line-height: var(--vs-type-line-height-helper);
  -webkit-box-orient: vertical;
  -webkit-line-clamp: 2;
}

.metric-chart__value {
  flex: none;
  color: var(--metric-color);
  font-size: clamp(1.15rem, 2vw, 1.55rem);
  font-variant-numeric: tabular-nums;
  line-height: 1;
}

.metric-chart__plot {
  height: 7.75rem;
  padding: 0 var(--vs-space-12);
}

.metric-chart__plot svg {
  width: 100%;
  height: 100%;
  overflow: visible;
}

.metric-chart__grid {
  stroke: var(--vs-color-border-subtle);
  stroke-width: 1;
  vector-effect: non-scaling-stroke;
}

.metric-chart__target {
  stroke: var(--vs-color-text-muted);
  stroke-width: 1;
  stroke-dasharray: 4 5;
  opacity: 0.55;
  vector-effect: non-scaling-stroke;
}

.metric-chart__line {
  fill: none;
  stroke: var(--metric-color);
  stroke-linecap: round;
  stroke-linejoin: round;
  stroke-width: 2;
  vector-effect: non-scaling-stroke;
}

.metric-chart__last-point {
  fill: var(--vs-color-bg-surface);
  stroke: var(--metric-color);
  stroke-width: 2;
  vector-effect: non-scaling-stroke;
}

.metric-chart__empty-line {
  stroke: var(--vs-color-border-strong);
  stroke-dasharray: 3 6;
  vector-effect: non-scaling-stroke;
}

.metric-chart__footer {
  display: flex;
  justify-content: space-between;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-16) var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
}

@media (prefers-reduced-motion: no-preference) {
  .metric-chart__line {
    transition: points var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
  }
}
</style>
