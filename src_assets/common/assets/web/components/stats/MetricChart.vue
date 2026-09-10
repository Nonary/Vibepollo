<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, useId } from 'vue';
import { useI18n } from 'vue-i18n';

import { AppButton } from '@/components/ui';

import type { ChartEvent } from './eventUtils';
import {
  clampChartRange,
  panChartRange,
  zoomChartRange,
  type ChartTimeRange,
} from './chartViewport';
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
    events?: ChartEvent[];
    expandable?: boolean;
    rangeLabel?: string;
  }>(),
  {
    values: () => [],
    unit: '',
    description: '',
    color: 'var(--vs-color-accent-default)',
    events: () => [],
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
const focusedIndex = ref<number | null>(null);
const focusedEventId = ref<string | null>(null);
const viewRange = ref<{ start: number; end: number } | null>(null);
let restoreFocusTo: HTMLElement | null = null;
const activePointers = new Map<number, { x: number; y: number }>();
let panState:
  | {
      pointerId: number;
      startX: number;
      startRange: { start: number; end: number };
      moved: boolean;
    }
  | undefined;
let pinchState:
  | {
      startDistance: number;
      startRange: { start: number; end: number };
    }
  | undefined;
let suppressClickUntil = 0;

const sourcePoints = computed<ChartValuePoint[]>(() => {
  if (props.points?.length) return props.points;
  return props.values.map((value, index) => ({
    timestamp: index,
    value: Number.isFinite(value) ? value : null,
  }));
});

const fullTimeDomain = computed(() => {
  const timestamps = sourcePoints.value
    .map((point) => point.timestamp)
    .filter((timestamp) => Number.isFinite(timestamp));
  timestamps.push(
    ...props.events
      .map((event) => event.timestamp)
      .filter((timestamp) => Number.isFinite(timestamp)),
  );
  const minimum = timestamps.length ? Math.min(...timestamps) : 0;
  const maximum = timestamps.length ? Math.max(...timestamps) : minimum + 1;
  return { start: minimum, end: maximum === minimum ? minimum + 1 : maximum };
});

function clampRange(range: ChartTimeRange): ChartTimeRange {
  return clampChartRange(fullTimeDomain.value, range);
}

const timeDomain = computed(() => clampRange(viewRange.value ?? fullTimeDomain.value));
const zoom = computed(() => {
  const full = fullTimeDomain.value;
  const visible = timeDomain.value;
  return Math.max(1, Math.min(12, (full.end - full.start) / (visible.end - visible.start)));
});

const visiblePoints = computed(() => {
  const domain = timeDomain.value;
  return sourcePoints.value.filter(
    (point) =>
      Number.isFinite(point.timestamp) &&
      point.timestamp >= domain.start &&
      point.timestamp <= domain.end,
  );
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

const gapThreshold = computed(() => {
  const deltas: number[] = [];
  // Derive cadence from the complete source. A zoomed window may contain two
  // samples on either side of a long outage; deriving the median from just
  // those points would make the outage look like a continuous line.
  const points = sourcePoints.value;
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
  const span = timeDomain.value.end - timeDomain.value.start;
  return paddingX + ((timestamp - timeDomain.value.start) / span) * (chartWidth - paddingX * 2);
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
    const timestamp = domain.start + (domain.end - domain.start) * fraction;
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

function axisLabelStyle(axis: { x: number }, index: number): Record<string, string> {
  const left = `${(axis.x / chartWidth) * 100}%`;
  if (index === 0) return { left, textAlign: 'left' };
  if (index === 2) return { left, transform: 'translateX(-100%)', textAlign: 'right' };
  return { left, transform: 'translateX(-50%)', textAlign: 'center' };
}

function eventLabelStyle(marker: { x: number }): Record<string, string> {
  const left = `${(marker.x / chartWidth) * 100}%`;
  if (marker.x <= paddingX + 24) return { left, textAlign: 'left' };
  if (marker.x >= chartWidth - paddingX - 24)
    return { left, transform: 'translateX(-100%)', textAlign: 'right' };
  return { left, transform: 'translateX(-50%)', textAlign: 'center' };
}

function inspect(point: ChartValuePoint, index: number): void {
  focusedIndex.value = index;
  focusedEventId.value = null;
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

const eventMarkers = computed(() => {
  const domain = timeDomain.value;
  const visible = props.events.filter(
    (event) => event.timestamp >= domain.start && event.timestamp <= domain.end,
  );
  const ordered = [...visible].sort((a, b) => a.timestamp - b.timestamp);
  let lastX = -Infinity;
  return ordered.map((event) => {
    const x = xFor(event.timestamp);
    const showLabel = x - lastX >= 34;
    if (showLabel) lastX = x;
    return { event, x, showLabel };
  });
});

const inspectedEvent = computed(
  () => props.events.find((event) => event.id === focusedEventId.value) ?? null,
);

function eventLabel(event: ChartEvent): string {
  const timestamp = formatTimestamp(event.timestamp);
  const type = event.eventType.replace(/_/g, ' ');
  return event.payload ? `${timestamp}: ${type} — ${event.payload}` : `${timestamp}: ${type}`;
}

function inspectEvent(event: ChartEvent): void {
  focusedEventId.value = event.id;
  focusedIndex.value = null;
}

function rangeZoomedAround(
  range: { start: number; end: number },
  timestamp: number,
  factor: number,
): { start: number; end: number } {
  return zoomChartRange(fullTimeDomain.value, range, timestamp, factor);
}

function zoomAround(timestamp: number, factor: number): void {
  setViewRange(rangeZoomedAround(timeDomain.value, timestamp, factor));
}

function setViewRange(next: ChartTimeRange): void {
  viewRange.value =
    next.start === fullTimeDomain.value.start && next.end === fullTimeDomain.value.end
      ? null
      : next;
}

function zoomIn(): void {
  zoomAround((timeDomain.value.start + timeDomain.value.end) / 2, 1.35);
}

function zoomOut(): void {
  zoomAround((timeDomain.value.start + timeDomain.value.end) / 2, 1 / 1.35);
}

function zoomReset(): void {
  viewRange.value = null;
  focusedIndex.value = null;
  focusedEventId.value = null;
}

function timestampForClientX(clientX: number, element: SVGSVGElement): number {
  const rect = element.getBoundingClientRect();
  const fraction = rect.width ? Math.max(0, Math.min(1, (clientX - rect.left) / rect.width)) : 0.5;
  const x = fraction * chartWidth;
  return (
    timeDomain.value.start +
    ((x - paddingX) / (chartWidth - paddingX * 2)) * (timeDomain.value.end - timeDomain.value.start)
  );
}

function pointerDistance(): number {
  const pointers = [...activePointers.values()];
  const first = pointers[0];
  const second = pointers[1];
  if (!first || !second) return 0;
  return Math.hypot(second.x - first.x, second.y - first.y);
}

function pointerCenterX(): number {
  const pointers = [...activePointers.values()];
  return pointers.reduce((sum, pointer) => sum + pointer.x, 0) / Math.max(1, pointers.length);
}

function onPlotPointerDown(event: PointerEvent): void {
  const svg = event.currentTarget as SVGSVGElement;
  if (event.pointerType === 'touch') {
    activePointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (activePointers.size === 2) {
      pinchState = {
        startDistance: Math.max(1, pointerDistance()),
        startRange: { ...timeDomain.value },
      };
      event.preventDefault();
    }
    return;
  }
  if (event.button !== 0 || !event.shiftKey) return;
  panState = {
    pointerId: event.pointerId,
    startX: event.clientX,
    startRange: { ...timeDomain.value },
    moved: false,
  };
  svg.setPointerCapture?.(event.pointerId);
  event.preventDefault();
}

function onPlotPointerMove(event: PointerEvent): void {
  if (event.pointerType === 'touch') {
    if (!activePointers.has(event.pointerId)) return;
    activePointers.set(event.pointerId, { x: event.clientX, y: event.clientY });
    if (!pinchState || activePointers.size < 2) return;
    const distance = Math.max(1, pointerDistance());
    const factor = distance / pinchState.startDistance;
    const svg = event.currentTarget as SVGSVGElement;
    const anchor = timestampForClientX(pointerCenterX(), svg);
    setViewRange(rangeZoomedAround(pinchState.startRange, anchor, factor));
    event.preventDefault();
    return;
  }
  if (!panState || panState.pointerId !== event.pointerId) return;
  const svg = event.currentTarget as SVGSVGElement;
  const rect = svg.getBoundingClientRect();
  const pixels = rect.width ? rect.width : chartWidth;
  const delta =
    ((event.clientX - panState.startX) / pixels) *
    (panState.startRange.end - panState.startRange.start);
  const next = panChartRange(fullTimeDomain.value, panState.startRange, -delta);
  panState.moved ||= Math.abs(event.clientX - panState.startX) > 3;
  setViewRange(next);
  event.preventDefault();
}

function onPlotPointerUp(event: PointerEvent): void {
  if (event.pointerType === 'touch') {
    activePointers.delete(event.pointerId);
    if (activePointers.size < 2) pinchState = undefined;
    return;
  }
  if (panState?.pointerId !== event.pointerId) return;
  if (panState.moved) suppressClickUntil = performance.now() + 150;
  (event.currentTarget as SVGSVGElement).releasePointerCapture?.(event.pointerId);
  panState = undefined;
}

function onPlotPointerCancel(event: PointerEvent): void {
  if (event.pointerType === 'touch') {
    activePointers.delete(event.pointerId);
    pinchState = undefined;
  } else if (panState?.pointerId === event.pointerId) {
    panState = undefined;
  }
}

function onPlotWheel(event: WheelEvent): void {
  if (!event.deltaY) return;
  const svg = event.currentTarget as SVGSVGElement;
  zoomAround(timestampForClientX(event.clientX, svg), event.deltaY < 0 ? 1.2 : 1 / 1.2);
  event.preventDefault();
}

function inspectOnClick(point: ChartValuePoint, index: number): void {
  if (performance.now() < suppressClickUntil) return;
  inspect(point, index);
}

async function openExpanded(): Promise<void> {
  if (document.activeElement instanceof HTMLElement) restoreFocusTo = document.activeElement;
  expanded.value = true;
  await nextTick();
  if (dialog.value && !dialog.value.open) {
    dialog.value.showModal();
    dialog.value.querySelector<HTMLElement>('button')?.focus();
  }
}

function closeExpanded(): void {
  if (dialog.value?.open) dialog.value.close();
  expanded.value = false;
  zoomReset();
  const restore = restoreFocusTo;
  restoreFocusTo = null;
  nextTick(() => restore?.focus());
}

function onNativeCancel(event: Event): void {
  event.preventDefault();
  closeExpanded();
}

onBeforeUnmount(() => {
  if (dialog.value?.open) dialog.value.close();
  activePointers.clear();
  panState = undefined;
  pinchState = undefined;
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
      <div class="metric-chart__surface">
        <svg
          viewBox="0 0 640 220"
          preserveAspectRatio="none"
          role="img"
          class="metric-chart__svg"
          :aria-label="`${title}: ${value}`"
          :data-view-start="timeDomain.start"
          :data-view-end="timeDomain.end"
          @wheel="onPlotWheel"
          @pointerdown="onPlotPointerDown"
          @pointermove="onPlotPointerMove"
          @pointerup="onPlotPointerUp"
          @pointercancel="onPlotPointerCancel"
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
          <line
            v-if="target != null"
            x1="42"
            :y1="yFor(target)"
            x2="638"
            :y2="yFor(target)"
            class="metric-chart__target"
          />
          <g
            v-for="marker in eventMarkers"
            :key="`event-${marker.event.id}`"
            class="metric-chart__event"
            role="button"
            tabindex="0"
            :aria-label="eventLabel(marker.event)"
            @click.stop="inspectEvent(marker.event)"
            @focus="inspectEvent(marker.event)"
            @keydown.enter.prevent="inspectEvent(marker.event)"
            @keydown.space.prevent="inspectEvent(marker.event)"
          >
            <title>{{ eventLabel(marker.event) }}</title>
            <line :x1="marker.x" y1="18" :x2="marker.x" y2="202" />
          </g>
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
            @click="inspectOnClick(entry.point, entry.index)"
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
        </svg>
        <span class="metric-chart__y-axis metric-chart__y-axis--top" aria-hidden="true">
          {{ upperBound.toLocaleString(locale, { maximumFractionDigits: 1 }) }}{{ unit }}
        </span>
        <span class="metric-chart__y-axis metric-chart__y-axis--bottom" aria-hidden="true">
          0{{ unit }}
        </span>
        <div class="metric-chart__event-labels" aria-hidden="true">
          <span
            v-for="marker in eventMarkers"
            v-show="marker.showLabel"
            :key="`event-label-${marker.event.id}`"
            class="metric-chart__event-label"
            :style="eventLabelStyle(marker)"
            >{{ marker.event.eventType.replace(/_/g, ' ') }}</span
          >
        </div>
      </div>
      <div class="metric-chart__axis-row" aria-hidden="true">
        <span
          v-for="(axis, index) in axisLabels"
          :key="axis.label"
          class="metric-chart__axis"
          :style="axisLabelStyle(axis, index)"
          >{{ axis.label }}</span
        >
      </div>
    </div>

    <div v-if="inspectedPoint" class="metric-chart__inspection" aria-live="polite">
      {{ pointLabel(inspectedPoint) }}
    </div>
    <div v-if="inspectedEvent" class="metric-chart__event-inspection" aria-live="polite">
      {{ eventLabel(inspectedEvent) }}
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
            <span class="metric-chart__zoom-hint">{{ t('sessions.chart_zoom_hint') }}</span>
          </div>
          <div class="metric-chart__dialog-plot">
            <div class="metric-chart__surface">
              <svg
                viewBox="0 0 640 220"
                preserveAspectRatio="none"
                role="img"
                class="metric-chart__svg"
                :aria-label="`${title}: ${value}`"
                :data-view-start="timeDomain.start"
                :data-view-end="timeDomain.end"
                @wheel="onPlotWheel"
                @pointerdown="onPlotPointerDown"
                @pointermove="onPlotPointerMove"
                @pointerup="onPlotPointerUp"
                @pointercancel="onPlotPointerCancel"
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
                <line
                  v-if="target != null"
                  x1="42"
                  :y1="yFor(target)"
                  x2="638"
                  :y2="yFor(target)"
                  class="metric-chart__target"
                />
                <g
                  v-for="marker in eventMarkers"
                  :key="`dialog-event-${marker.event.id}`"
                  class="metric-chart__event"
                  role="button"
                  tabindex="0"
                  :aria-label="eventLabel(marker.event)"
                  @click.stop="inspectEvent(marker.event)"
                  @focus="inspectEvent(marker.event)"
                  @keydown.enter.prevent="inspectEvent(marker.event)"
                  @keydown.space.prevent="inspectEvent(marker.event)"
                >
                  <title>{{ eventLabel(marker.event) }}</title>
                  <line :x1="marker.x" y1="18" :x2="marker.x" y2="202" />
                </g>
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
                  @click="inspectOnClick(entry.point, entry.index)"
                  @keydown.enter.prevent="inspect(entry.point, entry.index)"
                  @keydown.space.prevent="inspect(entry.point, entry.index)"
                />
              </svg>
              <span class="metric-chart__y-axis metric-chart__y-axis--top" aria-hidden="true">
                {{ upperBound.toLocaleString(locale, { maximumFractionDigits: 1 }) }}{{ unit }}
              </span>
              <span class="metric-chart__y-axis metric-chart__y-axis--bottom" aria-hidden="true">
                0{{ unit }}
              </span>
              <div class="metric-chart__event-labels" aria-hidden="true">
                <span
                  v-for="marker in eventMarkers"
                  v-show="marker.showLabel"
                  :key="`dialog-event-label-${marker.event.id}`"
                  class="metric-chart__event-label"
                  :style="eventLabelStyle(marker)"
                  >{{ marker.event.eventType.replace(/_/g, ' ') }}</span
                >
              </div>
            </div>
            <div class="metric-chart__axis-row" aria-hidden="true">
              <span
                v-for="(axis, index) in axisLabels"
                :key="axis.label"
                class="metric-chart__axis"
                :style="axisLabelStyle(axis, index)"
                >{{ axis.label }}</span
              >
            </div>
          </div>
          <div v-if="inspectedPoint" class="metric-chart__inspection" aria-live="polite">
            {{ pointLabel(inspectedPoint) }}
          </div>
          <div v-if="inspectedEvent" class="metric-chart__event-inspection" aria-live="polite">
            {{ eventLabel(inspectedEvent) }}
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
  display: flex;
  flex-direction: column;
  height: 11.5rem;
  padding: 0 var(--vs-space-12);
}
.metric-chart__dialog-plot {
  height: min(60vh, 32rem);
  min-height: 18rem;
}
.metric-chart__surface {
  position: relative;
  flex: 1;
  min-height: 0;
}
.metric-chart__plot svg,
.metric-chart__dialog-plot svg {
  display: block;
  width: 100%;
  height: 100%;
  overflow: visible;
  touch-action: pan-y;
  user-select: none;
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
.metric-chart__event {
  cursor: pointer;
  outline: none;
}
.metric-chart__event line {
  stroke: var(--vs-color-status-warning);
  stroke-width: 1.5;
  stroke-dasharray: 4 4;
  opacity: 0.78;
  vector-effect: non-scaling-stroke;
}
.metric-chart__event:hover line,
.metric-chart__event:focus line {
  stroke: var(--vs-color-focus-ring, var(--vs-color-status-warning));
  stroke-width: 2.5;
  opacity: 1;
}
.metric-chart__event:focus-visible {
  outline: var(--vs-focus-width) solid var(--vs-focus-ring);
  outline-offset: 2px;
}
.metric-chart__event-labels {
  position: absolute;
  inset: 0;
  pointer-events: none;
}
.metric-chart__event-label {
  fill: var(--vs-color-text-secondary);
  font-size: 9px;
  pointer-events: none;
}
.metric-chart__event-labels .metric-chart__event-label {
  position: absolute;
  top: 0.25rem;
  max-width: 9rem;
  overflow: hidden;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  line-height: 1.1;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.metric-chart__event-inspection {
  margin: 0 var(--vs-space-16);
  padding: var(--vs-space-6) var(--vs-space-8);
  border-radius: var(--vs-radius-control);
  background: color-mix(in srgb, var(--vs-color-status-warning) 10%, transparent);
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
}
.metric-chart__axis {
  fill: var(--vs-color-text-muted);
  font-size: 13px;
}
.metric-chart__y-axis {
  position: absolute;
  left: 0;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  line-height: 1;
  pointer-events: none;
  white-space: nowrap;
}
.metric-chart__y-axis--top {
  top: 0.15rem;
}
.metric-chart__y-axis--bottom {
  bottom: 0.15rem;
}
.metric-chart__axis-row {
  position: relative;
  flex: none;
  height: 1.5rem;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-variant-numeric: tabular-nums;
  line-height: 1.25rem;
  white-space: nowrap;
}
.metric-chart__axis-row .metric-chart__axis {
  position: absolute;
  top: 0;
  color: inherit;
  font-size: inherit;
  line-height: inherit;
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
.metric-chart__zoom-hint {
  margin-left: auto;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
@media (max-width: 639px) {
  .metric-chart__plot {
    height: 10.5rem;
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
  .metric-chart__zoom-actions {
    justify-content: flex-start;
    flex-wrap: wrap;
  }
  .metric-chart__zoom-hint {
    flex-basis: 100%;
    margin-left: 0;
    text-align: left;
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
