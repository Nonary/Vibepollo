<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, useId } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton } from '@/components/ui';

import type { ChartValuePoint } from './types';

const props = withDefaults(
  defineProps<{
    title: string;
    value: string;
    values?: number[];
    points?: ChartValuePoint[];
    unit?: string;
    description?: string;
    color?: string;
    ceiling?: number;
    target?: number;
    expandable?: boolean;
    rangeLabel?: string;
  }>(),
  {
    values: () => [],
    unit: '',
    description: '',
    color: 'var(--vs-color-accent-default)',
    expandable: true,
    rangeLabel: '',
  },
);

const chartWidth = 640;
const chartHeight = 220;
const paddingX = 42;
const paddingY = 18;
const uid = useId().replace(/:/g, '');
const { t, locale } = useI18n();
const expanded = ref(false);
const dialog = ref<HTMLDialogElement | null>(null);
const zoom = ref(1);
const focusedIndex = ref<number | null>(null);

const sourcePoints = computed<ChartValuePoint[]>(() => {
  if (props.points?.length) return props.points;
  return props.values.map((value, index) => ({
    timestamp: index,
    value: Number.isFinite(value) ? value : null,
  }));
});

const visiblePoints = computed(() => {
  const points = sourcePoints.value;
  if (zoom.value <= 1 || points.length < 3) return points;
  const count = Math.max(3, Math.ceil(points.length / zoom.value));
  const focus = focusedIndex.value == null ? points.length - 1 : focusedIndex.value;
  const start = Math.max(0, Math.min(points.length - count, focus - Math.floor(count / 2)));
  return points.slice(start, start + count);
});

const finiteVisiblePoints = computed(() =>
  visiblePoints.value.filter(
    (point) =>
      Number.isFinite(point.timestamp) && point.value != null && Number.isFinite(point.value),
  ),
);

const upperBound = computed(() => {
  if (props.ceiling && props.ceiling > 0) return props.ceiling;
  const largest = Math.max(
    ...finiteVisiblePoints.value.map((point) => point.value ?? 0),
    props.target ?? 0,
    1,
  );
  return largest * 1.12;
});

const timeDomain = computed(() => {
  // Keep missing-value timestamps in the domain. A null sample is a gap in
  // telemetry, not permission to compress the remaining values together.
  const timestamps = visiblePoints.value
    .map((point) => point.timestamp)
    .filter((timestamp) => Number.isFinite(timestamp));
  const minimum = timestamps.length ? Math.min(...timestamps) : 0;
  const maximum = timestamps.length ? Math.max(...timestamps) : minimum + 1;
  return { minimum, maximum: maximum === minimum ? minimum + 1 : maximum };
});

const gapThreshold = computed(() => {
  const deltas: number[] = [];
  const points = visiblePoints.value;
  for (let index = 1; index < points.length; index += 1) {
    const previous = points[index - 1];
    const current = points[index];
    if (!previous || !current || previous.segment !== current.segment) continue;
    const delta = current.timestamp - previous.timestamp;
    if (delta > 0) deltas.push(delta);
  }
  if (!deltas.length) return Number.POSITIVE_INFINITY;
  deltas.sort((a, b) => a - b);
  const median = deltas[Math.floor(deltas.length / 2)] ?? deltas[0] ?? 0;
  return Math.max(15_000, median * 3);
});

function xFor(timestamp: number): number {
  const span = timeDomain.value.maximum - timeDomain.value.minimum;
  return paddingX + ((timestamp - timeDomain.value.minimum) / span) * (chartWidth - paddingX * 2);
}

function yFor(value: number): number {
  const height = chartHeight - paddingY * 2;
  return paddingY + height - (Math.max(0, value) / upperBound.value) * height;
}

const pointCoordinates = computed<
  Array<{ point: ChartValuePoint; index: number; x: number; y: number }>
>(() =>
  visiblePoints.value
    .map((point, index) => ({
      point,
      index,
      x: xFor(point.timestamp),
      y:
        !Number.isFinite(point.timestamp) || point.value == null || !Number.isFinite(point.value)
          ? null
          : Math.max(paddingY, yFor(point.value)),
    }))
    .flatMap((entry) => (entry.y == null ? [] : [{ ...entry, y: entry.y }])),
);

const lineSegments = computed(() => {
  const segments: Array<Array<{ x: number; y: number }>> = [];
  let current: Array<{ x: number; y: number }> = [];
  let previous: ChartValuePoint | undefined;
  for (const point of visiblePoints.value) {
    const numeric =
      Number.isFinite(point.timestamp) && point.value != null && Number.isFinite(point.value);
    const gap = previous && point.timestamp - previous.timestamp > gapThreshold.value;
    const streamChanged = previous && point.segment != null && previous.segment !== point.segment;
    if (!numeric || gap || streamChanged) {
      if (current.length) segments.push(current);
      current = [];
    }
    if (numeric)
      current.push({ x: xFor(point.timestamp), y: Math.max(paddingY, yFor(point.value!)) });
    previous = point;
  }
  if (current.length) segments.push(current);
  return segments;
});

function linePath(segment: Array<{ x: number; y: number }>): string {
  return segment
    .map((point, index) => `${index ? 'L' : 'M'} ${point.x.toFixed(2)} ${point.y.toFixed(2)}`)
    .join(' ');
}

const axisLabels = computed(() => {
  const domain = timeDomain.value;
  return [0, 0.5, 1].map((fraction) => {
    const timestamp = domain.minimum + (domain.maximum - domain.minimum) * fraction;
    return { x: xFor(timestamp), label: formatTimestamp(timestamp) };
  });
});

function formatTimestamp(timestamp: number): string {
  if (timestamp < 100_000_000) return `#${Math.round(timestamp) + 1}`;
  return new Intl.DateTimeFormat(locale.value, {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  }).format(new Date(timestamp));
}

function inspect(point: ChartValuePoint, index: number): void {
  focusedIndex.value = index;
  if (point.value == null || !Number.isFinite(point.value)) return;
}

function pointLabel(point: ChartValuePoint): string {
  const value =
    point.value == null || !Number.isFinite(point.value)
      ? t('sessions.history_no_samples')
      : `${point.value.toLocaleString(locale.value, { maximumFractionDigits: 2 })}${props.unit}`;
  return `${formatTimestamp(point.timestamp)}: ${value}`;
}

const inspectedPoint = computed(() => {
  const index = focusedIndex.value;
  if (index == null) return null;
  const point = visiblePoints.value[index];
  return point && point.value != null && Number.isFinite(point.value) ? point : null;
});

function zoomIn(): void {
  zoom.value = Math.min(12, Math.round(zoom.value * 1.35 * 100) / 100);
}

function zoomOut(): void {
  zoom.value = Math.max(1, Math.round((zoom.value / 1.35) * 100) / 100);
}

function zoomReset(): void {
  zoom.value = 1;
  focusedIndex.value = null;
}

async function openExpanded(): Promise<void> {
  expanded.value = true;
  await nextTick();
  if (dialog.value && !dialog.value.open) dialog.value.showModal();
}

function closeExpanded(): void {
  if (dialog.value?.open) dialog.value.close();
  expanded.value = false;
  zoomReset();
}

function onNativeCancel(event: Event): void {
  event.preventDefault();
  closeExpanded();
}

onBeforeUnmount(() => {
  if (dialog.value?.open) dialog.value.close();
});
</script>

<template>
  <article class="metric-chart" :style="{ '--metric-color': color }">
    <header class="metric-chart__header">
      <div>
        <h4 :title="description">{{ title }}</h4>
        <p v-if="description">{{ description }}</p>
      </div>
      <div class="metric-chart__header-actions">
        <span v-if="rangeLabel" class="metric-chart__range">{{ rangeLabel }}</span>
        <strong class="metric-chart__value">{{ value }}</strong>
        <AppButton
          v-if="expandable"
          :label="t('sessions.chart_expand')"
          icon="external-link"
          icon-only
          variant="tertiary"
          size="compact"
          @click="openExpanded"
        />
      </div>
    </header>

    <div class="metric-chart__plot">
      <svg
        viewBox="0 0 640 220"
        preserveAspectRatio="none"
        role="img"
        :aria-label="`${title}: ${value}`"
      >
        <line
          v-for="grid in [62, 112, 162]"
          :key="grid"
          x1="42"
          :y1="grid"
          x2="638"
          :y2="grid"
          class="metric-chart__grid"
        />
        <text x="4" y="22" class="metric-chart__axis">
          {{ upperBound.toLocaleString(locale, { maximumFractionDigits: 1 }) }}{{ unit }}
        </text>
        <text x="4" y="210" class="metric-chart__axis">0{{ unit }}</text>
        <line
          v-if="target != null"
          x1="42"
          :y1="yFor(target)"
          x2="638"
          :y2="yFor(target)"
          class="metric-chart__target"
        />
        <path
          v-for="(segment, index) in lineSegments"
          :key="index"
          :d="linePath(segment)"
          class="metric-chart__line"
        />
        <circle
          v-for="entry in pointCoordinates"
          :key="`${entry.point.timestamp}:${entry.index}`"
          :cx="entry.x"
          :cy="entry.y"
          r="5"
          class="metric-chart__point"
          tabindex="0"
          role="button"
          :aria-label="pointLabel(entry.point)"
          @focus="inspect(entry.point, entry.index)"
          @pointerdown="inspect(entry.point, entry.index)"
          @keydown.enter.prevent="inspect(entry.point, entry.index)"
          @keydown.space.prevent="inspect(entry.point, entry.index)"
        />
        <line
          v-if="!pointCoordinates.length"
          x1="42"
          y1="162"
          x2="638"
          y2="162"
          class="metric-chart__empty-line"
        />
        <text
          v-for="axis in axisLabels"
          :key="axis.label"
          :x="axis.x"
          y="218"
          text-anchor="middle"
          class="metric-chart__axis"
        >
          {{ axis.label }}
        </text>
      </svg>
    </div>

    <div v-if="inspectedPoint" class="metric-chart__inspection" aria-live="polite">
      {{ pointLabel(inspectedPoint) }}
    </div>
    <footer class="metric-chart__footer">
      <span>{{
        t('ui.stats.minimum', {
          value: `${finiteVisiblePoints.length ? Math.min(...finiteVisiblePoints.map((point) => point.value!)).toLocaleString(locale, { maximumFractionDigits: 1 }) : '—'}${unit}`,
        })
      }}</span>
      <span>{{
        t(
          'ui.stats.sample_count',
          { count: finiteVisiblePoints.length },
          finiteVisiblePoints.length,
        )
      }}</span>
      <span>{{
        t('ui.stats.maximum', {
          value: `${finiteVisiblePoints.length ? Math.max(...finiteVisiblePoints.map((point) => point.value!)).toLocaleString(locale, { maximumFractionDigits: 1 }) : '—'}${unit}`,
        })
      }}</span>
    </footer>

    <Teleport to="body">
      <dialog
        ref="dialog"
        class="metric-chart__dialog"
        :style="{ '--metric-color': color }"
        @cancel="onNativeCancel"
      >
        <section class="metric-chart__dialog-panel" :aria-labelledby="`metric-chart-title-${uid}`">
          <header class="metric-chart__dialog-header">
            <div>
              <h2 :id="`metric-chart-title-${uid}`">{{ title }}</h2>
              <p v-if="description">{{ description }}</p>
            </div>
            <AppButton
              :label="t('_common.close')"
              icon="x"
              icon-only
              variant="tertiary"
              @click="closeExpanded"
            />
          </header>
          <div class="metric-chart__zoom-actions">
            <AppButton
              :label="t('sessions.chart_zoom_out')"
              icon="minus"
              size="compact"
              variant="secondary"
              :disabled="zoom <= 1"
              @click="zoomOut"
            />
            <span aria-live="polite">{{ Math.round(zoom * 100) }}%</span>
            <AppButton
              :label="t('sessions.chart_zoom_in')"
              icon="plus"
              size="compact"
              variant="secondary"
              :disabled="zoom >= 12"
              @click="zoomIn"
            />
            <AppButton
              :label="t('sessions.chart_zoom_reset')"
              size="compact"
              variant="tertiary"
              :disabled="zoom === 1"
              @click="zoomReset"
            />
          </div>
          <div class="metric-chart__dialog-plot">
            <svg
              viewBox="0 0 640 220"
              preserveAspectRatio="none"
              role="img"
              :aria-label="`${title}: ${value}`"
            >
              <line
                v-for="grid in [62, 112, 162]"
                :key="grid"
                x1="42"
                :y1="grid"
                x2="638"
                :y2="grid"
                class="metric-chart__grid"
              />
              <text x="4" y="22" class="metric-chart__axis">
                {{ upperBound.toLocaleString(locale, { maximumFractionDigits: 1 }) }}{{ unit }}
              </text>
              <text x="4" y="210" class="metric-chart__axis">0{{ unit }}</text>
              <path
                v-for="(segment, index) in lineSegments"
                :key="index"
                :d="linePath(segment)"
                class="metric-chart__line"
              />
              <circle
                v-for="entry in pointCoordinates"
                :key="`${entry.point.timestamp}:${entry.index}`"
                :cx="entry.x"
                :cy="entry.y"
                r="5"
                class="metric-chart__point"
                tabindex="0"
                role="button"
                :aria-label="pointLabel(entry.point)"
                @focus="inspect(entry.point, entry.index)"
                @pointerdown="inspect(entry.point, entry.index)"
                @keydown.enter.prevent="inspect(entry.point, entry.index)"
                @keydown.space.prevent="inspect(entry.point, entry.index)"
              />
              <text
                v-for="axis in axisLabels"
                :key="axis.label"
                :x="axis.x"
                y="218"
                text-anchor="middle"
                class="metric-chart__axis"
              >
                {{ axis.label }}
              </text>
            </svg>
          </div>
          <div v-if="inspectedPoint" class="metric-chart__inspection" aria-live="polite">
            {{ pointLabel(inspectedPoint) }}
          </div>
        </section>
      </dialog>
    </Teleport>
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
  flex-wrap: wrap;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-12);
  padding: var(--vs-space-16) var(--vs-space-16) var(--vs-space-8);
}
.metric-chart__header > div:first-child {
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
.metric-chart__header-actions {
  display: flex;
  min-width: 0;
  align-items: center;
  gap: var(--vs-space-8);
  margin-left: auto;
}
.metric-chart__range {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  white-space: nowrap;
}
.metric-chart__value {
  flex: none;
  color: var(--metric-color);
  font-size: clamp(1.15rem, 2vw, 1.55rem);
  font-variant-numeric: tabular-nums;
  line-height: 1;
}
.metric-chart__plot,
.metric-chart__dialog-plot {
  height: 10rem;
  padding: 0 var(--vs-space-12);
}
.metric-chart__dialog-plot {
  height: min(60vh, 32rem);
  min-height: 18rem;
}
.metric-chart__plot svg,
.metric-chart__dialog-plot svg {
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
  stroke-width: 2.25;
  vector-effect: non-scaling-stroke;
}
.metric-chart__point {
  fill: var(--vs-color-bg-surface);
  stroke: var(--metric-color);
  stroke-width: 2;
  vector-effect: non-scaling-stroke;
  cursor: crosshair;
}
.metric-chart__point:focus {
  outline: none;
  stroke: var(--vs-color-focus-ring, var(--metric-color));
  stroke-width: 4;
}
.metric-chart__axis {
  fill: var(--vs-color-text-muted);
  font-size: 10px;
}
.metric-chart__empty-line {
  stroke: var(--vs-color-border-strong);
  stroke-dasharray: 3 6;
  vector-effect: non-scaling-stroke;
}
.metric-chart__inspection {
  margin: 0 var(--vs-space-16);
  padding: var(--vs-space-6) var(--vs-space-8);
  border-radius: var(--vs-radius-control);
  background: color-mix(in srgb, var(--metric-color) 10%, transparent);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
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
.metric-chart__dialog {
  width: min(94vw, 72rem);
  max-width: none;
  padding: 0;
  border: 1px solid var(--vs-color-border-strong);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-canvas);
  color: var(--vs-color-text-primary);
  box-shadow: var(--vs-shadow-overlay);
}
.metric-chart__dialog::backdrop {
  background: rgb(0 0 0 / 0.7);
  backdrop-filter: blur(3px);
}
.metric-chart__dialog-panel {
  display: grid;
  gap: var(--vs-space-16);
  padding: var(--vs-space-20);
}
.metric-chart__dialog-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-16);
}
.metric-chart__dialog-header h2 {
  font-size: var(--vs-type-size-section);
}
.metric-chart__dialog-header p {
  margin-top: var(--vs-space-4);
  color: var(--vs-color-text-secondary);
}
.metric-chart__zoom-actions {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: var(--vs-space-8);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
@media (max-width: 639px) {
  .metric-chart__plot {
    height: 9rem;
  }
  .metric-chart__dialog {
    width: 100vw;
    height: 100dvh;
    border: 0;
    border-radius: 0;
  }
  .metric-chart__dialog-panel {
    min-height: 100%;
    padding: var(--vs-space-16);
  }
  .metric-chart__dialog-plot {
    height: 42vh;
    min-height: 14rem;
  }
  .metric-chart__header {
    padding-inline: var(--vs-space-12);
  }
  .metric-chart__header-actions {
    flex-wrap: wrap;
    justify-content: flex-end;
  }
}
</style>
