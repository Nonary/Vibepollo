<template>
  <div class="space-y-4 mt-4">
    <div class="chart-range-toolbar">
      <span class="chart-range-label">
        <i class="fas fa-clock-rotate-left" />
        {{ t('sessions.chart_range') }}
      </span>
      <n-button-group size="tiny">
        <n-button
          v-for="option in rangeOptions"
          :key="option.key"
          size="tiny"
          :type="isSelectedRange(option.value) ? 'primary' : 'default'"
          :secondary="isSelectedRange(option.value)"
          :title="option.title"
          @click="selectedRangeMinutes = option.value"
        >
          {{ option.label }}
        </n-button>
      </n-button-group>
    </div>

    <SessionChartPanel
      v-if="protocol === 'rtsp'"
      icon="fas fa-clock"
      :title="t('sessions.chart_encode_latency')"
      :tip="t('sessions.tip_chart_encode_latency')"
      :subtitle="t('sessions.chart_ms')"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('latency')"
    >
      <ChartLine :data="latencyChartData" :options="latencyChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-tachometer-alt"
      :title="t('sessions.chart_throughput')"
      :tip="t('sessions.tip_chart_throughput')"
      subtitle="Mbps"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('throughput')"
    >
      <ChartLine :data="throughputChartData" :options="baseChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-exclamation-triangle"
      :title="t('sessions.chart_quality')"
      :tip="t('sessions.tip_chart_quality')"
      :subtitle="t('sessions.chart_events')"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('quality')"
    >
      <ChartLine :data="qualityChartData" :options="qualityChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-film"
      :title="t('sessions.chart_framerate')"
      :tip="t('sessions.tip_chart_framerate')"
      subtitle="FPS"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('fps')"
    >
      <ChartLine :data="fpsChartData" :options="fpsChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      v-if="hasHostCompute"
      icon="fas fa-microchip"
      :title="t('sessions.chart_host_compute')"
      :tip="t('sessions.tip_chart_host_compute')"
      subtitle="%"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('host_compute')"
    >
      <ChartLine :data="hostComputeChartData" :options="hostPercentChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      v-if="hasHostMemory"
      icon="fas fa-memory"
      :title="t('sessions.chart_host_memory')"
      :tip="t('sessions.tip_chart_host_memory')"
      subtitle="%"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('host_memory')"
    >
      <ChartLine :data="hostMemoryChartData" :options="hostPercentChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      v-if="hasHostNetwork"
      icon="fas fa-network-wired"
      :title="t('sessions.chart_host_network')"
      :tip="t('sessions.tip_chart_host_network')"
      subtitle="Mbps"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('host_network')"
    >
      <ChartLine :data="hostNetworkChartData" :options="hostNetworkChartOptions" />
    </SessionChartPanel>

    <SessionChartZoomModal
      :show="zoomVisible"
      :title="zoomTitle"
      :hint="t('sessions.chart_zoom_hint')"
      :zoom-in-title="t('sessions.chart_zoom_in')"
      :zoom-out-title="t('sessions.chart_zoom_out')"
      :zoom-reset-title="t('sessions.chart_zoom_reset')"
      @update:show="zoomVisible = $event"
      @zoom-in="zoomIn"
      @zoom-out="zoomOut"
      @zoom-reset="zoomReset"
      @after-leave="zoomReset"
    >
      <ChartLine
        v-if="zoomChart === 'latency'"
        ref="modalChartRef"
        :data="latencyChartData"
        :options="latencyChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'throughput'"
        ref="modalChartRef"
        :data="throughputChartData"
        :options="throughputChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'quality'"
        ref="modalChartRef"
        :data="qualityChartData"
        :options="qualityChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'fps'"
        ref="modalChartRef"
        :data="fpsChartData"
        :options="fpsChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'host_compute'"
        ref="modalChartRef"
        :data="hostComputeChartData"
        :options="hostPercentChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'host_memory'"
        ref="modalChartRef"
        :data="hostMemoryChartData"
        :options="hostPercentChartOptionsZoom"
      />
      <ChartLine
        v-else-if="zoomChart === 'host_network'"
        ref="modalChartRef"
        :data="hostNetworkChartData"
        :options="hostNetworkChartOptionsZoom"
      />
    </SessionChartZoomModal>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch, watchEffect } from 'vue';
import { useI18n } from 'vue-i18n';
import { NButton, NButtonGroup } from 'naive-ui';
import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  Filler,
  Tooltip,
  Legend,
} from 'chart.js';
import annotationPlugin from 'chartjs-plugin-annotation';
import zoomPlugin from 'chartjs-plugin-zoom';
import { Line } from 'vue-chartjs';
import type { SessionSample, SessionEvent } from '@/types/sessions';
import { fetchSessionDetail } from '@/services/sessionsApi';
import SessionChartPanel from './session/SessionChartPanel.vue';
import SessionChartZoomModal from './session/SessionChartZoomModal.vue';
import {
  buildFpsChartData,
  buildHostComputeChartData,
  buildHostMemoryChartData,
  buildHostNetworkChartData,
  buildLatencyChartData,
  buildQualityChartData,
  buildThroughputChartData,
} from './session/sessionChartDatasets';
import {
  buildBaseChartOptions,
  buildFpsChartOptions,
  buildHostNetworkChartOptions,
  buildHostPercentChartOptions,
  buildLatencyChartOptions,
  buildQualityChartOptions,
  withZoom,
} from './session/sessionChartOptions';
import { useSessionChartHistory, type SessionSnapshot } from './session/useSessionChartHistory';

ChartJS.register(
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  Filler,
  Tooltip,
  Legend,
  annotationPlugin,
  zoomPlugin,
);

const { t, locale } = useI18n();
type LooseLineComponent = new () => {
  $props: {
    data?: unknown;
    options?: unknown;
  };
};
const ChartLine = Line as LooseLineComponent;

const props = defineProps<{
  session?: SessionSnapshot;
  sessionId?: string;
  protocol?: 'rtsp' | 'webrtc';
  mode?: 'live' | 'history';
  historyData?: SessionSample[];
  events?: SessionEvent[];
}>();

const LIVE_HISTORY_REFRESH_MS = 15000;
const RECENT_RANGE_MINUTES = [3, 5, 15, 30, 60] as const;
const selectedRangeMinutes = ref<number | null>(null);
const liveHistoryData = ref<SessionSample[]>([]);
const liveEvents = ref<SessionEvent[]>([]);
const isLiveMode = computed(() => props.mode !== 'history');

let liveHistoryTimer: ReturnType<typeof setInterval> | undefined;
let liveHistoryRequestToken = 0;

const rangeOptions = computed(() => [
  {
    key: 'full',
    value: null as number | null,
    label: t('sessions.chart_range_full'),
    title: t('sessions.chart_range_full_title'),
  },
  ...RECENT_RANGE_MINUTES.map((minutes) => ({
    key: `recent-${minutes}`,
    value: minutes as number | null,
    label: t('sessions.chart_range_recent_short', { minutes }),
    title: t('sessions.chart_range_recent_title', { minutes }),
  })),
]);

const chartHistoryData = computed(() =>
  isLiveMode.value ? liveHistoryData.value : props.historyData,
);
const chartEvents = computed(() => (isLiveMode.value ? liveEvents.value : props.events));

function isSelectedRange(value: number | null): boolean {
  return selectedRangeMinutes.value === value;
}

async function refreshLiveHistory(): Promise<void> {
  const sessionId = props.sessionId;
  if (!isLiveMode.value || !sessionId) {
    return;
  }

  const requestToken = ++liveHistoryRequestToken;
  try {
    const detail = await fetchSessionDetail(sessionId, { full: true });
    if (
      requestToken !== liveHistoryRequestToken ||
      props.sessionId !== sessionId ||
      !isLiveMode.value
    ) {
      return;
    }
    liveHistoryData.value = detail.samples ?? [];
    liveEvents.value = detail.events ?? [];
  } catch {
    // Session history can be disabled or the active session may not have been
    // persisted yet. Keep the in-memory live chart as the fallback.
  }
}

function stopLiveHistoryPolling(): void {
  if (liveHistoryTimer !== undefined) {
    clearInterval(liveHistoryTimer);
    liveHistoryTimer = undefined;
  }
  liveHistoryRequestToken += 1;
}

function startLiveHistoryPolling(): void {
  stopLiveHistoryPolling();
  liveHistoryData.value = [];
  liveEvents.value = [];
  if (!isLiveMode.value || !props.sessionId) {
    return;
  }

  void refreshLiveHistory();
  liveHistoryTimer = setInterval(() => {
    void refreshLiveHistory();
  }, LIVE_HISTORY_REFRESH_MS);
}

watch([() => props.sessionId, isLiveMode], startLiveHistoryPolling, { immediate: true });
onBeforeUnmount(stopLiveHistoryPolling);

const chartHistoryProps = reactive<Parameters<typeof useSessionChartHistory>[0]>({});
watchEffect(() => {
  if (props.session) {
    chartHistoryProps.session = props.session;
  } else {
    delete chartHistoryProps.session;
  }

  if (props.sessionId) {
    chartHistoryProps.sessionId = props.sessionId;
  } else {
    delete chartHistoryProps.sessionId;
  }

  if (props.protocol) {
    chartHistoryProps.protocol = props.protocol;
  } else {
    delete chartHistoryProps.protocol;
  }

  if (props.mode) {
    chartHistoryProps.mode = props.mode;
  } else {
    delete chartHistoryProps.mode;
  }

  const historyData = chartHistoryData.value;
  if (historyData) {
    chartHistoryProps.historyData = historyData;
  } else {
    delete chartHistoryProps.historyData;
  }

  const events = chartEvents.value;
  if (events) {
    chartHistoryProps.events = events;
  } else {
    delete chartHistoryProps.events;
  }

  chartHistoryProps.windowMinutes = selectedRangeMinutes.value;
  chartHistoryProps.locale = locale.value;
});

const {
  displayData,
  labels,
  hasHostSeries,
  hasHostCompute,
  hasHostMemory,
  hasHostNetwork,
  eventAnnotations,
} = useSessionChartHistory(chartHistoryProps);

const baseChartOptions = computed(() => buildBaseChartOptions(eventAnnotations.value));
const latencyChartOptions = computed(() => buildLatencyChartOptions(baseChartOptions.value));
const qualityChartOptions = computed(() => buildQualityChartOptions(baseChartOptions.value));
const fpsChartOptions = computed(() =>
  buildFpsChartOptions(baseChartOptions.value, props.session?.fps ?? 60),
);

const latencyChartData = computed(() => buildLatencyChartData(labels.value, displayData.value, t));
const throughputChartData = computed(() =>
  buildThroughputChartData(labels.value, displayData.value, t),
);
const qualityChartData = computed(() =>
  buildQualityChartData(labels.value, displayData.value, props.protocol ?? 'rtsp', t),
);
const fpsChartData = computed(() => buildFpsChartData(labels.value, displayData.value, t));

// Host stats charts (history mode only)

const hostPercentChartOptions = computed(() =>
  buildHostPercentChartOptions(baseChartOptions.value),
);

const hostComputeChartData = computed(() =>
  buildHostComputeChartData(labels.value, displayData.value, hasHostSeries, t),
);

const hostMemoryChartData = computed(() =>
  buildHostMemoryChartData(labels.value, displayData.value, hasHostSeries, t),
);

const hostNetworkChartOptions = computed(() =>
  buildHostNetworkChartOptions(baseChartOptions.value),
);

const hostNetworkChartData = computed(() =>
  buildHostNetworkChartData(labels.value, displayData.value, hasHostSeries, t),
);

// Chart zoom modal
type ZoomKey =
  | 'latency'
  | 'throughput'
  | 'quality'
  | 'fps'
  | 'host_compute'
  | 'host_memory'
  | 'host_network';
const zoomVisible = ref(false);
const zoomChart = ref<ZoomKey>('throughput');
const modalChartRef = ref<InstanceType<typeof Line>>();

const latencyChartOptionsZoom = computed(() => withZoom(latencyChartOptions.value));
const throughputChartOptionsZoom = computed(() => withZoom(baseChartOptions.value));
const qualityChartOptionsZoom = computed(() => withZoom(qualityChartOptions.value));
const fpsChartOptionsZoom = computed(() => withZoom(fpsChartOptions.value));
const hostPercentChartOptionsZoom = computed(() => withZoom(hostPercentChartOptions.value));
const hostNetworkChartOptionsZoom = computed(() => withZoom(hostNetworkChartOptions.value));

interface ZoomableChart {
  zoom: (f: number) => void;
  resetZoom: () => void;
}

function modalChartInstance(): ZoomableChart | false {
  const inst = modalChartRef.value as unknown as { chart?: ZoomableChart };
  return inst?.chart ?? false;
}

function zoomIn(): void {
  const c = modalChartInstance();
  if (c) c.zoom(1.2);
}
function zoomOut(): void {
  const c = modalChartInstance();
  if (c) c.zoom(0.8);
}
function zoomReset(): void {
  const c = modalChartInstance();
  if (c) c.resetZoom();
}

const zoomTitle = computed(() => {
  switch (zoomChart.value) {
    case 'latency':
      return t('sessions.chart_encode_latency');
    case 'throughput':
      return t('sessions.chart_throughput');
    case 'quality':
      return t('sessions.chart_quality');
    case 'fps':
      return t('sessions.chart_framerate');
    case 'host_compute':
      return t('sessions.chart_host_compute');
    case 'host_memory':
      return t('sessions.chart_host_memory');
    case 'host_network':
      return t('sessions.chart_host_network');
    default:
      return '';
  }
});

function openZoom(key: ZoomKey): void {
  zoomChart.value = key;
  zoomVisible.value = true;
}
</script>

<style scoped>
.chart-range-toolbar {
  @apply flex flex-wrap items-center justify-between gap-2 rounded-xl border border-dark/[0.06] bg-light/[0.03] px-3 py-2 dark:border-light/[0.10] dark:bg-dark/[0.06];
}
.chart-range-label {
  @apply inline-flex items-center gap-2 text-xs font-semibold uppercase tracking-wider opacity-70;
}
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
.chart-wrapper-zoom {
  height: 60vh;
  min-height: 360px;
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
.zoom-hint {
  @apply mt-2 text-[11px] opacity-60 text-center flex items-center justify-center gap-2;
}
</style>
