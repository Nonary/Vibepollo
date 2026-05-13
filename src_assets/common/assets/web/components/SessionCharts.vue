<template>
  <div class="space-y-4 mt-4">
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
      v-if="mode === 'history' && hasHostCompute"
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
      v-if="mode === 'history' && hasHostMemory"
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
      v-if="mode === 'history' && hasHostNetwork"
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
import { computed, ref } from 'vue';
import { useI18n } from 'vue-i18n';
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
import SessionChartPanel from './session/SessionChartPanel.vue';
import SessionChartZoomModal from './session/SessionChartZoomModal.vue';
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

const { t } = useI18n();
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
const {
  displayData,
  labels,
  hasHostSeries,
  hasHostCompute,
  hasHostMemory,
  hasHostNetwork,
  eventAnnotations,
} = useSessionChartHistory(props);

const baseChartOptions = computed(() => buildBaseChartOptions(eventAnnotations.value));
const latencyChartOptions = computed(() => buildLatencyChartOptions(baseChartOptions.value));
const qualityChartOptions = computed(() => buildQualityChartOptions(baseChartOptions.value));
const fpsChartOptions = computed(() =>
  buildFpsChartOptions(baseChartOptions.value, props.session?.fps ?? 60),
);

// Chart data

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const latencyChartData = computed<any>(() => ({
  labels: labels.value,
  datasets: [
    {
      label: t('sessions.chart_encode_latency'),
      data: displayData.value.map((p) => p.encode_latency_ms),
      borderColor: 'rgb(59, 130, 246)',
      backgroundColor: 'rgba(59, 130, 246, 0.1)',
      fill: true,
    },
  ],
}));

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const throughputChartData = computed<any>(() => ({
  labels: labels.value,
  datasets: [
    {
      label: t('sessions.chart_throughput'),
      data: displayData.value.map((p) => p.throughput_mbps),
      borderColor: 'rgb(16, 185, 129)',
      backgroundColor: 'rgba(16, 185, 129, 0.1)',
      fill: true,
    },
  ],
}));

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const qualityChartData = computed<any>(() => {
  const isRtsp = props.protocol !== 'webrtc';
  return {
    labels: labels.value,
    datasets: [
      {
        label: isRtsp ? t('sessions.client_losses') : t('sessions.video_dropped'),
        data: displayData.value.map((p) => p.delta_losses),
        borderColor: 'rgb(239, 68, 68)',
        backgroundColor: 'rgba(239, 68, 68, 0.15)',
        fill: true,
      },
      {
        label: isRtsp ? t('sessions.idr_requests') : t('sessions.audio_dropped'),
        data: displayData.value.map((p) => p.delta_idr),
        borderColor: 'rgb(245, 158, 11)',
        backgroundColor: 'rgba(245, 158, 11, 0.15)',
        fill: true,
      },
      ...(isRtsp
        ? [
            {
              label: t('sessions.frame_invalidations'),
              data: displayData.value.map((p) => p.delta_invalidations),
              borderColor: 'rgb(168, 85, 247)',
              backgroundColor: 'rgba(168, 85, 247, 0.15)',
              fill: true,
            },
          ]
        : []),
    ],
  };
});

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const fpsChartData = computed<any>(() => ({
  labels: labels.value,
  datasets: [
    {
      label: t('sessions.chart_framerate'),
      data: displayData.value.map((p) => p.actual_fps),
      borderColor: 'rgb(236, 72, 153)',
      backgroundColor: 'rgba(236, 72, 153, 0.1)',
      fill: true,
    },
  ],
}));

// Host stats charts (history mode only)

const hostPercentChartOptions = computed(() =>
  buildHostPercentChartOptions(baseChartOptions.value),
);

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const hostComputeChartData = computed<any>(() => {
  const datasets = [];
  if (hasHostSeries('host_cpu_percent')) {
    datasets.push({
      label: t('sessions.chart_host_cpu'),
      data: displayData.value.map((p) => p.host_cpu_percent),
      borderColor: 'rgb(59, 130, 246)',
      backgroundColor: 'rgba(59, 130, 246, 0.12)',
      fill: true,
    });
  }
  if (hasHostSeries('host_gpu_percent')) {
    datasets.push({
      label: t('sessions.chart_host_gpu'),
      data: displayData.value.map((p) => p.host_gpu_percent),
      borderColor: 'rgb(16, 185, 129)',
      backgroundColor: 'rgba(16, 185, 129, 0.12)',
      fill: true,
    });
  }
  if (hasHostSeries('host_gpu_encoder_percent')) {
    datasets.push({
      label: t('sessions.chart_host_gpu_encoder'),
      data: displayData.value.map((p) => p.host_gpu_encoder_percent),
      borderColor: 'rgb(168, 85, 247)',
      backgroundColor: 'rgba(168, 85, 247, 0.12)',
      fill: true,
    });
  }
  return { labels: labels.value, datasets };
});

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const hostMemoryChartData = computed<any>(() => {
  const datasets = [];
  if (hasHostSeries('host_ram_percent')) {
    datasets.push({
      label: t('sessions.chart_host_ram'),
      data: displayData.value.map((p) => p.host_ram_percent),
      borderColor: 'rgb(245, 158, 11)',
      backgroundColor: 'rgba(245, 158, 11, 0.12)',
      fill: true,
    });
  }
  if (hasHostSeries('host_vram_percent')) {
    datasets.push({
      label: t('sessions.chart_host_vram'),
      data: displayData.value.map((p) => p.host_vram_percent),
      borderColor: 'rgb(236, 72, 153)',
      backgroundColor: 'rgba(236, 72, 153, 0.12)',
      fill: true,
    });
  }
  return { labels: labels.value, datasets };
});

const hostNetworkChartOptions = computed(() =>
  buildHostNetworkChartOptions(baseChartOptions.value),
);

function bpsToMbps(v: number): number {
  return Math.round((v / 1_000_000) * 100) / 100;
}

// eslint-disable-next-line @typescript-eslint/no-explicit-any -- vue-chartjs accepts the resulting Chart.js payload shape
const hostNetworkChartData = computed<any>(() => {
  const datasets = [];
  if (hasHostSeries('host_net_rx_bps')) {
    datasets.push({
      label: t('sessions.chart_host_net_rx'),
      data: displayData.value.map((p) => bpsToMbps(p.host_net_rx_bps)),
      borderColor: 'rgb(34, 197, 94)',
      backgroundColor: 'rgba(34, 197, 94, 0.12)',
      fill: true,
    });
  }
  if (hasHostSeries('host_net_tx_bps')) {
    datasets.push({
      label: t('sessions.chart_host_net_tx'),
      data: displayData.value.map((p) => bpsToMbps(p.host_net_tx_bps)),
      borderColor: 'rgb(59, 130, 246)',
      backgroundColor: 'rgba(59, 130, 246, 0.12)',
      fill: true,
    });
  }
  return { labels: labels.value, datasets };
});

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
