<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { apiGet } from '@/api/client';
import type { SessionDetail, SessionEvent, SessionSample } from '@/types/sessions';

import MetricChart from './MetricChart.vue';
import {
  filterChartRange,
  samplesToHostSeries,
  samplesToPerformancePoints,
  type HostChartSeries,
} from './historyUtils';
import type { ChartValuePoint, PerformancePoint } from './types';

const props = withDefaults(
  defineProps<{
    points: PerformancePoint[];
    protocol?: string;
    targetFps?: number;
    sessionId?: string;
    mode?: 'live' | 'history';
    hostSamples?: SessionSample[];
    events?: SessionEvent[];
    liveEnabled?: boolean;
    pauseWhenHidden?: boolean;
  }>(),
  {
    protocol: 'rtsp',
    targetFps: 0,
    sessionId: '',
    mode: 'history',
    hostSamples: () => [],
    events: () => [],
    liveEnabled: true,
    pauseWhenHidden: true,
  },
);

const { t } = useI18n();
const LIVE_HISTORY_REFRESH_MS = 15_000;
const rangeOptions = [null, 3, 5, 15, 30, 60] as const;
const selectedRangeMinutes = ref<number | null>(null);
const retainedSamples = ref<SessionSample[]>([]);
const retainedEvents = ref<SessionEvent[]>([]);
const retainedUnavailable = ref(false);
let liveHistoryTimer: ReturnType<typeof setInterval> | undefined;
let liveHistoryRequestToken = 0;

const isLive = computed(() => props.mode === 'live' && Boolean(props.sessionId));
const usingRetainedSamples = computed(() => isLive.value && retainedSamples.value.length > 0);
const sourcePoints = computed<PerformancePoint[]>(() => {
  const persisted = usingRetainedSamples.value
    ? samplesToPerformancePoints(retainedSamples.value, props.protocol)
    : [];
  if (!persisted.length) return props.points;
  const known = new Set(persisted.map((point) => `${point.timestamp}:${point.segment ?? ''}`));
  const merged = [
    ...persisted,
    ...props.points.filter((point) => !known.has(`${point.timestamp}:${point.segment ?? ''}`)),
  ];
  return merged.sort((a, b) => a.timestamp - b.timestamp);
});

const sourceHostSamples = computed(() =>
  props.hostSamples.length ? props.hostSamples : retainedSamples.value,
);
const hostSeries = computed<HostChartSeries>(() => samplesToHostSeries(sourceHostSamples.value));
const latestTimestamp = computed(() => {
  const points = [
    ...sourcePoints.value.map((point) => point.timestamp),
    ...sourceHostSamples.value.map((sample) => sample.timestamp_unix * 1000),
  ];
  return Math.max(...points, 0);
});

const displayPoints = computed(() =>
  filterChartRange(sourcePoints.value, selectedRangeMinutes.value, latestTimestamp.value),
);
const displayHostSeries = computed<HostChartSeries>(() => {
  const filter = (points: ChartValuePoint[]) =>
    filterChartRange(points, selectedRangeMinutes.value, latestTimestamp.value);
  return {
    cpu: filter(hostSeries.value.cpu),
    gpu: filter(hostSeries.value.gpu),
    encoder: filter(hostSeries.value.encoder),
    ram: filter(hostSeries.value.ram),
    vram: filter(hostSeries.value.vram),
    rx: filter(hostSeries.value.rx),
    tx: filter(hostSeries.value.tx),
  };
});

const latest = computed(() => displayPoints.value.at(-1));
const qualityLabel = computed(() => t('sessions.chart_quality'));
const hasHostSeries = computed(() =>
  Object.values(displayHostSeries.value).some((points: ChartValuePoint[]) =>
    points.some((point) => point.value != null),
  ),
);
const hasPerformanceValues = computed(() =>
  displayPoints.value.some((point) =>
    [
      point.latencyMs,
      point.throughputMbps,
      point.qualityEvents,
      point.videoDropped,
      point.audioDropped,
      point.fps,
    ].some((value) => value != null && Number.isFinite(value)),
  ),
);

function values(
  points: PerformancePoint[],
  getter: (point: PerformancePoint) => number | null,
): ChartValuePoint[] {
  return points.map((point) => ({
    timestamp: point.timestamp,
    value: getter(point),
    segment: point.segment,
  }));
}

function decimal(value: number | null | undefined, suffix: string, digits = 1): string {
  if (value == null || !Number.isFinite(value)) return '—';
  return `${value.toLocaleString(undefined, { maximumFractionDigits: digits })}${suffix}`;
}

function rangeLabel(): string {
  return selectedRangeMinutes.value == null
    ? t('sessions.chart_range_full')
    : t('sessions.chart_range_recent_short', { minutes: selectedRangeMinutes.value });
}

function selectRange(value: number | null): void {
  selectedRangeMinutes.value = value;
}

async function refreshLiveHistory(): Promise<void> {
  const sessionId = props.sessionId;
  if (!isLive.value || !props.liveEnabled || (props.pauseWhenHidden && document.hidden)) return;
  const token = ++liveHistoryRequestToken;
  try {
    const detail = await apiGet<SessionDetail>(
      `/api/history/sessions/${encodeURIComponent(sessionId)}?full=1`,
    );
    if (token !== liveHistoryRequestToken || props.sessionId !== sessionId || !isLive.value) return;
    // A full detail response is the retained session timeline. Keep it intact
    // so “Full session” can reach samples from before this page was opened;
    // local fallback points remain bounded by StatsView's chart preferences.
    retainedSamples.value = [...(detail.samples ?? [])].sort(
      (a, b) => a.timestamp_unix - b.timestamp_unix,
    );
    retainedEvents.value = detail.events ?? [];
    retainedUnavailable.value =
      retainedSamples.value.length === 0 || Boolean(detail.samples_truncated);
  } catch {
    if (token === liveHistoryRequestToken) retainedUnavailable.value = true;
    // The in-memory points remain visible when history is disabled or not yet committed.
  }
}

function stopLiveHistoryPolling(): void {
  if (liveHistoryTimer !== undefined) {
    clearInterval(liveHistoryTimer);
    liveHistoryTimer = undefined;
  }
  liveHistoryRequestToken += 1;
}

function onVisibilityChange(): void {
  if (!isLive.value || !props.liveEnabled) return;
  if (document.hidden && props.pauseWhenHidden) {
    stopLiveHistoryPolling();
  } else {
    startLiveHistoryPolling();
  }
}

function startLiveHistoryPolling(): void {
  stopLiveHistoryPolling();
  retainedSamples.value = [];
  retainedEvents.value = [];
  retainedUnavailable.value = false;
  if (!isLive.value || !props.liveEnabled) return;
  if (props.pauseWhenHidden) document.addEventListener('visibilitychange', onVisibilityChange);
  void refreshLiveHistory();
  liveHistoryTimer = setInterval(() => void refreshLiveHistory(), LIVE_HISTORY_REFRESH_MS);
}

watch(
  [() => props.sessionId, () => props.mode, () => props.liveEnabled, () => props.pauseWhenHidden],
  startLiveHistoryPolling,
  { immediate: true },
);

onBeforeUnmount(() => {
  stopLiveHistoryPolling();
  document.removeEventListener('visibilitychange', onVisibilityChange);
});
</script>

<template>
  <div class="performance-charts">
    <div class="chart-range-toolbar" role="group" :aria-label="t('sessions.chart_range')">
      <span class="chart-range-label">{{ t('sessions.chart_range') }}</span>
      <div class="chart-range-options">
        <button
          v-for="option in rangeOptions"
          :key="option == null ? 'full' : option"
          type="button"
          class="chart-range-button"
          :class="{ 'chart-range-button--selected': selectedRangeMinutes === option }"
          :aria-pressed="selectedRangeMinutes === option"
          :title="
            option == null
              ? t('sessions.chart_range_full_title')
              : t('sessions.chart_range_recent_title', { minutes: option })
          "
          @click="selectRange(option)"
        >
          {{
            option == null
              ? t('sessions.chart_range_full')
              : t('sessions.chart_range_recent_short', { minutes: option })
          }}
        </button>
      </div>
    </div>
    <p v-if="retainedUnavailable && isLive" class="chart-history-fallback" role="status">
      {{ t('ui.sessions.alert.partial_failure_description') }}
    </p>
    <p class="chart-history-source" role="status">
      {{ isLive && !usingRetainedSamples ? t('sessions.live') : t('sessions.history_title') }}
    </p>
    <p
      v-if="selectedRangeMinutes != null && !hasPerformanceValues && !hasHostSeries"
      class="chart-history-empty"
      role="status"
    >
      {{ t('sessions.history_no_samples') }}
    </p>

    <MetricChart
      v-if="protocol.toLocaleLowerCase() !== 'webrtc'"
      :title="t('sessions.chart_encode_latency')"
      :description="t('sessions.tip_chart_encode_latency')"
      :value="decimal(latest?.latencyMs, ' ms')"
      :points="values(displayPoints, (point) => point.latencyMs)"
      unit=" ms"
      color="var(--vs-color-status-info)"
      :target="16"
      :range-label="rangeLabel()"
    />
    <MetricChart
      :title="t('sessions.chart_throughput')"
      :description="t('sessions.tip_chart_throughput')"
      :value="decimal(latest?.throughputMbps, ' Mbps', 2)"
      :points="values(displayPoints, (point) => point.throughputMbps)"
      unit=" Mbps"
      color="var(--vs-color-status-success)"
      :range-label="rangeLabel()"
    />
    <MetricChart
      :title="qualityLabel"
      :description="t('sessions.tip_chart_quality')"
      :value="decimal(latest?.qualityEvents, '', 0)"
      :points="values(displayPoints, (point) => point.qualityEvents)"
      color="var(--vs-color-status-warning)"
      :range-label="rangeLabel()"
    />
    <template v-if="protocol.toLocaleLowerCase() === 'webrtc'">
      <MetricChart
        :title="t('sessions.video_dropped')"
        :value="decimal(latest?.videoDropped, '', 0)"
        :points="values(displayPoints, (point) => point.videoDropped ?? null)"
        unit=" events"
        color="var(--vs-color-status-danger)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        :title="t('sessions.audio_dropped')"
        :value="decimal(latest?.audioDropped, '', 0)"
        :points="values(displayPoints, (point) => point.audioDropped ?? null)"
        unit=" events"
        color="var(--vs-color-status-warning)"
        :range-label="rangeLabel()"
      />
    </template>
    <MetricChart
      :title="t('sessions.chart_framerate')"
      :description="t('sessions.tip_chart_framerate')"
      :value="decimal(latest?.fps, ' fps')"
      :points="values(displayPoints, (point) => point.fps)"
      unit=" fps"
      color="var(--vs-color-data-accent)"
      :target="targetFps || undefined"
      :range-label="rangeLabel()"
    />

    <template v-if="hasHostSeries">
      <MetricChart
        v-if="displayHostSeries.cpu.some((point) => point.value != null)"
        :title="t('sessions.chart_host_cpu')"
        :value="decimal(displayHostSeries.cpu.at(-1)?.value, '%', 1)"
        :points="displayHostSeries.cpu"
        unit="%"
        :ceiling="100"
        color="var(--vs-color-status-info)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.gpu.some((point) => point.value != null)"
        :title="t('sessions.chart_host_gpu')"
        :value="decimal(displayHostSeries.gpu.at(-1)?.value, '%', 1)"
        :points="displayHostSeries.gpu"
        unit="%"
        :ceiling="100"
        color="var(--vs-color-status-success)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.encoder.some((point) => point.value != null)"
        :title="t('sessions.chart_host_gpu_encoder')"
        :value="decimal(displayHostSeries.encoder.at(-1)?.value, '%', 1)"
        :points="displayHostSeries.encoder"
        unit="%"
        :ceiling="100"
        color="var(--vs-color-data-accent)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.ram.some((point) => point.value != null)"
        :title="t('sessions.chart_host_ram')"
        :value="decimal(displayHostSeries.ram.at(-1)?.value, '%', 1)"
        :points="displayHostSeries.ram"
        unit="%"
        :ceiling="100"
        color="var(--vs-color-status-warning)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.vram.some((point) => point.value != null)"
        :title="t('sessions.chart_host_vram')"
        :value="decimal(displayHostSeries.vram.at(-1)?.value, '%', 1)"
        :points="displayHostSeries.vram"
        unit="%"
        :ceiling="100"
        color="var(--vs-color-data-accent)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.rx.some((point) => point.value != null)"
        :title="t('sessions.chart_host_net_rx')"
        :value="decimal(displayHostSeries.rx.at(-1)?.value, ' Mbps', 2)"
        :points="displayHostSeries.rx"
        unit=" Mbps"
        color="var(--vs-color-status-success)"
        :range-label="rangeLabel()"
      />
      <MetricChart
        v-if="displayHostSeries.tx.some((point) => point.value != null)"
        :title="t('sessions.chart_host_net_tx')"
        :value="decimal(displayHostSeries.tx.at(-1)?.value, ' Mbps', 2)"
        :points="displayHostSeries.tx"
        unit=" Mbps"
        color="var(--vs-color-status-info)"
        :range-label="rangeLabel()"
      />
    </template>
  </div>
</template>

<style scoped>
.performance-charts {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-12);
}
.chart-range-toolbar {
  display: flex;
  grid-column: 1 / -1;
  flex-wrap: wrap;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
  padding: var(--vs-space-8) var(--vs-space-12);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: var(--vs-color-bg-surface);
}
.chart-range-label {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
}
.chart-range-options {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-4);
}
.chart-range-button {
  min-height: 2rem;
  padding: 0 var(--vs-space-8);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-control);
  background: transparent;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-helper);
  cursor: pointer;
}
.chart-range-button:hover,
.chart-range-button:focus-visible {
  border-color: var(--vs-color-accent-default);
  color: var(--vs-color-text-primary);
}
.chart-range-button--selected {
  border-color: var(--vs-color-accent-default);
  background: color-mix(in srgb, var(--vs-color-accent-default) 16%, transparent);
  color: var(--vs-color-text-primary);
}
.chart-history-fallback {
  grid-column: 1 / -1;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
.chart-history-source {
  grid-column: 1 / -1;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
.chart-history-empty {
  grid-column: 1 / -1;
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}
@media (max-width: 1199px) {
  .performance-charts {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}
@media (max-width: 639px) {
  .performance-charts {
    grid-template-columns: minmax(0, 1fr);
  }
  .chart-range-toolbar {
    align-items: flex-start;
    flex-direction: column;
  }
  .chart-range-options {
    width: 100%;
  }
  .chart-range-button {
    flex: 1 1 auto;
  }
}
</style>
