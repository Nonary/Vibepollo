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
      <Line :data="latencyChartData" :options="latencyChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-tachometer-alt"
      :title="t('sessions.chart_throughput')"
      :tip="t('sessions.tip_chart_throughput')"
      subtitle="Mbps"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('throughput')"
    >
      <Line :data="throughputChartData" :options="baseChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-exclamation-triangle"
      :title="t('sessions.chart_quality')"
      :tip="t('sessions.tip_chart_quality')"
      :subtitle="t('sessions.chart_events')"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('quality')"
    >
      <Line :data="qualityChartData" :options="qualityChartOptions" />
    </SessionChartPanel>

    <SessionChartPanel
      icon="fas fa-film"
      :title="t('sessions.chart_framerate')"
      :tip="t('sessions.tip_chart_framerate')"
      subtitle="FPS"
      :expand-title="t('sessions.chart_expand')"
      @expand="openZoom('fps')"
    >
      <Line :data="fpsChartData" :options="fpsChartOptions" />
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
      <Line :data="hostComputeChartData" :options="hostPercentChartOptions" />
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
      <Line :data="hostMemoryChartData" :options="hostPercentChartOptions" />
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
      <Line :data="hostNetworkChartData" :options="hostNetworkChartOptions" />
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
      <Line
        v-if="zoomChart === 'latency'"
        ref="modalChartRef"
        :data="latencyChartData"
        :options="latencyChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'throughput'"
        ref="modalChartRef"
        :data="throughputChartData"
        :options="throughputChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'quality'"
        ref="modalChartRef"
        :data="qualityChartData"
        :options="qualityChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'fps'"
        ref="modalChartRef"
        :data="fpsChartData"
        :options="fpsChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'host_compute'"
        ref="modalChartRef"
        :data="hostComputeChartData"
        :options="hostPercentChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'host_memory'"
        ref="modalChartRef"
        :data="hostMemoryChartData"
        :options="hostPercentChartOptionsZoom"
      />
      <Line
        v-else-if="zoomChart === 'host_network'"
        ref="modalChartRef"
        :data="hostNetworkChartData"
        :options="hostNetworkChartOptionsZoom"
      />
    </SessionChartZoomModal>
  </div>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue';
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

interface SessionSnapshot {
  // Common
  fps?: number;
  // RTSP-specific
  encode_latency_ms?: number;
  frames_sent?: number;
  packets_sent?: number;
  bytes_sent?: number;
  client_reported_losses?: number;
  idr_requests?: number;
  invalidate_ref_count?: number;
  // WebRTC-specific
  video_packets?: number;
  audio_packets?: number;
  video_dropped?: number;
  audio_dropped?: number;
  last_video_frame_index?: number;
}

const props = defineProps<{
  session?: SessionSnapshot;
  sessionId?: string;
  protocol?: 'rtsp' | 'webrtc';
  mode?: 'live' | 'history';
  historyData?: SessionSample[];
  events?: SessionEvent[];
}>();

const MAX_POINTS = 150; // 5 minutes at 2s polling

interface DataPoint {
  time: string;
  encode_latency_ms: number;
  throughput_mbps: number;
  delta_losses: number;
  delta_idr: number;
  delta_invalidations: number;
  actual_fps: number;
  host_cpu_percent: number;
  host_gpu_percent: number;
  host_gpu_encoder_percent: number;
  host_ram_percent: number;
  host_vram_percent: number;
  host_net_rx_bps: number;
  host_net_tx_bps: number;
}

function nz(v?: number): number {
  // Backend uses -1 to mean "not available"; treat that and missing values as 0 for charting.
  if (typeof v !== 'number') return 0;
  return v < 0 ? 0 : v;
}

const history = ref<DataPoint[]>([]);
// eslint-disable-next-line @typescript-eslint/ban-types, no-restricted-syntax -- local mutable state requires explicit undefined union
let prevSnapshot: SessionSnapshot | undefined;
let prevTimestamp = 0;
// eslint-disable-next-line @typescript-eslint/ban-types, no-restricted-syntax -- local mutable state requires explicit undefined union
let trackedSessionId: string | undefined;

function resetHistory(): void {
  history.value = [];
  prevSnapshot = undefined;
  prevTimestamp = 0;
}

function formatTime(date: Date): string {
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function convertHistoryData(samples: SessionSample[]): DataPoint[] {
  if (!samples.length) return [];
  return samples.map((sample, i) => {
    const prev = i > 0 ? samples[i - 1] : undefined;
    return {
      time: formatTime(new Date(sample.timestamp_unix * 1000)),
      encode_latency_ms: sample.encode_latency_ms,
      throughput_mbps: Math.round((sample.actual_bitrate_kbps / 1000) * 100) / 100,
      delta_losses: prev
        ? Math.max(0, sample.client_reported_losses - prev.client_reported_losses)
        : 0,
      delta_idr: prev ? Math.max(0, sample.idr_requests - prev.idr_requests) : 0,
      delta_invalidations: prev
        ? Math.max(0, sample.ref_invalidations - prev.ref_invalidations)
        : 0,
      actual_fps: Math.round(sample.actual_fps * 10) / 10,
      host_cpu_percent: nz(sample.host_cpu_percent),
      host_gpu_percent: nz(sample.host_gpu_percent),
      host_gpu_encoder_percent: nz(sample.host_gpu_encoder_percent),
      host_ram_percent: nz(sample.host_ram_percent),
      host_vram_percent: nz(sample.host_vram_percent),
      host_net_rx_bps: nz(sample.host_net_rx_bps),
      host_net_tx_bps: nz(sample.host_net_tx_bps),
    };
  });
}

function hasHostSeries(
  field: keyof Pick<
    SessionSample,
    | 'host_cpu_percent'
    | 'host_gpu_percent'
    | 'host_gpu_encoder_percent'
    | 'host_ram_percent'
    | 'host_vram_percent'
    | 'host_net_rx_bps'
    | 'host_net_tx_bps'
  >,
): boolean {
  if (props.mode !== 'history' || !props.historyData?.length) return false;
  return props.historyData.some((s) => {
    const v = s[field];
    return typeof v === 'number' && v >= 0;
  });
}

const hasHostCompute = computed(
  () =>
    hasHostSeries('host_cpu_percent') ||
    hasHostSeries('host_gpu_percent') ||
    hasHostSeries('host_gpu_encoder_percent'),
);
const hasHostMemory = computed(
  () => hasHostSeries('host_ram_percent') || hasHostSeries('host_vram_percent'),
);
const hasHostNetwork = computed(
  () => hasHostSeries('host_net_rx_bps') || hasHostSeries('host_net_tx_bps'),
);

const displayData = computed<DataPoint[]>(() => {
  if (props.mode === 'history' && props.historyData) {
    return convertHistoryData(props.historyData);
  }
  return history.value;
});

watch(
  () => props.session,
  (session) => {
    if (props.mode === 'history') return;
    if (!session) return;

    // Reset history when the session identity changes to prevent cross-session delta contamination
    const currentId = props.sessionId;
    if (currentId !== trackedSessionId) {
      resetHistory();
      trackedSessionId = currentId;
      // Record the first snapshot but skip delta computation
      prevSnapshot = { ...session };
      prevTimestamp = Date.now();
      return;
    }

    const now = Date.now();
    const timeLabel = formatTime(new Date());
    const isRtsp = props.protocol !== 'webrtc';

    let throughput_mbps = 0;
    let delta_losses = 0;
    let delta_idr = 0;
    let delta_invalidations = 0;
    let actual_fps = 0;

    if (prevSnapshot && prevTimestamp > 0) {
      const dt = (now - prevTimestamp) / 1000; // seconds
      if (dt > 0) {
        if (isRtsp) {
          const deltaBytes = (session.bytes_sent ?? 0) - (prevSnapshot.bytes_sent ?? 0);
          throughput_mbps = Math.max(0, (deltaBytes * 8) / (dt * 1_000_000));
          delta_losses = Math.max(
            0,
            (session.client_reported_losses ?? 0) - (prevSnapshot.client_reported_losses ?? 0),
          );
          delta_idr = Math.max(0, (session.idr_requests ?? 0) - (prevSnapshot.idr_requests ?? 0));
          delta_invalidations = Math.max(
            0,
            (session.invalidate_ref_count ?? 0) - (prevSnapshot.invalidate_ref_count ?? 0),
          );
          const deltaFrames = (session.frames_sent ?? 0) - (prevSnapshot.frames_sent ?? 0);
          actual_fps = Math.max(0, deltaFrames / dt);
        } else {
          // WebRTC: backend now exposes cumulative bytes_sent (video+audio)
          // and last_video_frame_index, so use real Mbps + FPS like RTSP.
          const deltaBytes = (session.bytes_sent ?? 0) - (prevSnapshot.bytes_sent ?? 0);
          throughput_mbps = Math.max(0, (deltaBytes * 8) / (dt * 1_000_000));
          delta_losses = Math.max(
            0,
            (session.video_dropped ?? 0) - (prevSnapshot.video_dropped ?? 0),
          );
          delta_idr = Math.max(0, (session.audio_dropped ?? 0) - (prevSnapshot.audio_dropped ?? 0));
          const prevFrame = prevSnapshot.last_video_frame_index ?? 0;
          const curFrame = session.last_video_frame_index ?? 0;
          const deltaFrames = curFrame - prevFrame;
          actual_fps = Math.max(0, deltaFrames / dt);
        }
      }
    }

    const point: DataPoint = {
      time: timeLabel,
      encode_latency_ms: session.encode_latency_ms ?? 0,
      throughput_mbps: Math.round(throughput_mbps * 100) / 100,
      delta_losses,
      delta_idr,
      delta_invalidations,
      actual_fps: Math.round(actual_fps * 10) / 10,
      host_cpu_percent: 0,
      host_gpu_percent: 0,
      host_gpu_encoder_percent: 0,
      host_ram_percent: 0,
      host_vram_percent: 0,
      host_net_rx_bps: 0,
      host_net_tx_bps: 0,
    };

    const h = [...history.value, point];
    if (h.length > MAX_POINTS) h.shift();
    history.value = h;

    prevSnapshot = { ...session };
    prevTimestamp = now;
  },
  { deep: true },
);

onBeforeUnmount(() => {
  resetHistory();
  trackedSessionId = undefined;
});

const labels = computed(() => displayData.value.map((p) => p.time));

// Event annotation colors by event type
const eventColors: Record<string, string> = {
  stream_started: 'rgba(34, 197, 94, 0.6)',
  stream_ended: 'rgba(59, 130, 246, 0.6)',
  first_drop: 'rgba(239, 68, 68, 0.7)',
  drop_burst: 'rgba(239, 68, 68, 0.5)',
  stall: 'rgba(245, 158, 11, 0.7)',
  recovery: 'rgba(34, 197, 94, 0.5)',
};

function findClosestSampleIndex(samples: SessionSample[], timestampUnix: number): number {
  if (!samples.length) {
    return 0;
  }

  let low = 0;
  let high = samples.length - 1;

  while (low < high) {
    const mid = Math.floor((low + high) / 2);
    const midTimestamp = samples[mid]?.timestamp_unix ?? 0;
    if (midTimestamp < timestampUnix) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  const upperIndex = low;
  if (upperIndex <= 0) {
    return 0;
  }

  const lowerIndex = upperIndex - 1;
  const lowerDiff = Math.abs(timestampUnix - (samples[lowerIndex]?.timestamp_unix ?? 0));
  const upperDiff = Math.abs(timestampUnix - (samples[upperIndex]?.timestamp_unix ?? 0));
  return lowerDiff <= upperDiff ? lowerIndex : upperIndex;
}

// Build annotation lines from events in history mode
const eventAnnotations = computed(() => {
  if (props.mode !== 'history' || !props.events?.length || !props.historyData?.length) return {};
  const data = displayData.value;
  if (!data.length) return {};

  const samples = props.historyData ?? [];

  const resolved = props.events.map((evt, i) => {
    const closest = findClosestSampleIndex(samples, evt.timestamp_unix);
    return { i, evt, closest };
  });

  // Sort by index so we can detect clusters of nearby labels and avoid
  // overlapping text by alternating label position (and dropping label
  // text entirely when more than 2 events fall within CLUSTER_WINDOW
  // points of each other).
  const ordered = [...resolved].sort((a, b) => a.closest - b.closest);
  const CLUSTER_WINDOW = 3;
  const labelDecisions = new Map<number, { show: boolean; position: 'start' | 'end' }>();
  let lastIdx = -Infinity;
  let runLength = 0;
  let alternate: 'start' | 'end' = 'start';
  for (const item of ordered) {
    if (item.closest - lastIdx <= CLUSTER_WINDOW) {
      runLength++;
      alternate = alternate === 'start' ? 'end' : 'start';
    } else {
      runLength = 1;
      alternate = 'start';
    }
    // In a tight cluster of 3+ events, hide labels beyond the second to
    // avoid an unreadable pile-up; the colored line + tooltip still tell
    // the story.
    labelDecisions.set(item.i, { show: runLength <= 2, position: alternate });
    lastIdx = item.closest;
  }

  const annotations: Record<string, unknown> = {};
  for (const { i, evt, closest } of resolved) {
    const decision = labelDecisions.get(i) ?? { show: true, position: 'start' as const };
    annotations[`event-${i}`] = {
      type: 'line',
      xMin: closest,
      xMax: closest,
      borderColor: eventColors[evt.event_type] ?? 'rgba(128, 128, 128, 0.5)',
      borderWidth: 2,
      borderDash: [4, 4],
      label: {
        display: decision.show,
        content: evt.event_type.replace(/_/g, ' '),
        position: decision.position,
        backgroundColor: eventColors[evt.event_type] ?? 'rgba(128, 128, 128, 0.8)',
        color: '#fff',
        font: { size: 9 },
        padding: 3,
      },
    };
  }
  return annotations;
});

// Shared styling
const gridColor = 'rgba(128, 128, 128, 0.15)';
const tickColor = 'rgba(128, 128, 128, 0.6)';

const baseChartOptions = computed(() => {
  const hasAnnotations = Object.keys(eventAnnotations.value).length > 0;
  return {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 0 },
    interaction: { mode: 'index' as const, intersect: false },
    plugins: {
      legend: { display: false },
      tooltip: {
        backgroundColor: 'rgba(0, 0, 0, 0.8)',
        titleColor: '#fff',
        bodyColor: '#fff',
        padding: 8,
        cornerRadius: 6,
      },
      ...(hasAnnotations ? { annotation: { annotations: eventAnnotations.value } } : {}),
    },
    scales: {
      x: {
        grid: { color: gridColor },
        ticks: { color: tickColor, maxTicksLimit: 8, maxRotation: 0, font: { size: 10 } },
      },
      y: {
        beginAtZero: true,
        grid: { color: gridColor },
        ticks: { color: tickColor, font: { size: 10 } },
      },
    },
    elements: {
      point: { radius: 0, hitRadius: 8 },
      line: { tension: 0.3, borderWidth: 2 },
    },
  };
});

const latencyChartOptions = computed(() => {
  const base = { ...baseChartOptions.value };
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: { display: false },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: 20,
        ticks: { ...base.scales.y.ticks, callback: (v: number) => `${v}ms` },
      },
    },
  };
});

const qualityChartOptions = computed(() => {
  const base = { ...baseChartOptions.value };
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        ticks: {
          ...base.scales.y.ticks,
          stepSize: 1,
          callback: (v: number) => (Math.floor(v) === v ? v : ''),
        },
      },
    },
  };
});

const fpsChartOptions = computed(() => {
  const targetFps = props.session?.fps ?? 60;
  const base = { ...baseChartOptions.value };
  return {
    ...base,
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: targetFps + 5,
        ticks: { ...base.scales.y.ticks, callback: (v: number) => `${v}` },
      },
    },
  };
});

// Chart data

const latencyChartData = computed(() => ({
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

const throughputChartData = computed(() => ({
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

const qualityChartData = computed(() => {
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

const fpsChartData = computed(() => ({
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

const hostPercentChartOptions = computed(() => {
  const base = { ...baseChartOptions.value };
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        suggestedMax: 100,
        ticks: { ...base.scales.y.ticks, callback: (v: number) => `${v}%` },
      },
    },
  };
});

const hostComputeChartData = computed(() => {
  const datasets: Array<Record<string, unknown>> = [];
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

const hostMemoryChartData = computed(() => {
  const datasets: Array<Record<string, unknown>> = [];
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

const hostNetworkChartOptions = computed(() => {
  const base = { ...baseChartOptions.value };
  return {
    ...base,
    plugins: {
      ...base.plugins,
      legend: {
        display: true,
        position: 'top' as const,
        labels: { color: tickColor, boxWidth: 12, padding: 8, font: { size: 10 } },
      },
    },
    scales: {
      ...base.scales,
      y: {
        ...base.scales.y,
        beginAtZero: true,
        ticks: { ...base.scales.y.ticks, callback: (v: number) => `${v} Mbps` },
      },
    },
  };
});

function bpsToMbps(v: number): number {
  return Math.round((v / 1_000_000) * 100) / 100;
}

const hostNetworkChartData = computed(() => {
  const datasets: Array<Record<string, unknown>> = [];
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

const zoomPluginConfig = {
  pan: { enabled: true, mode: 'x' as const, modifierKey: 'shift' as const },
  zoom: {
    wheel: { enabled: true },
    pinch: { enabled: true },
    drag: { enabled: false },
    mode: 'x' as const,
  },
  limits: {
    x: { minRange: 2 },
  },
};

function withZoom<T extends { plugins?: Record<string, unknown> }>(opts: T): T {
  return {
    ...opts,
    plugins: {
      ...(opts.plugins ?? {}),
      zoom: zoomPluginConfig,
    },
  };
}

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
