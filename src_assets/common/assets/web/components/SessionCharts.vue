<template>
  <div class="space-y-4 mt-4">
    <!-- Encode Latency Chart (RTSP only) -->
    <div v-if="protocol === 'rtsp'" class="chart-container">
      <div class="chart-header">
        <span class="chart-title">
          <i class="fas fa-clock mr-1" />{{ t('sessions.chart_encode_latency') }}
        </span>
        <span class="chart-subtitle">{{ t('sessions.chart_ms') }}</span>
      </div>
      <div class="chart-wrapper">
        <Line :data="latencyChartData" :options="latencyChartOptions" />
      </div>
    </div>

    <!-- Throughput Chart -->
    <div class="chart-container">
      <div class="chart-header">
        <span class="chart-title">
          <i class="fas fa-tachometer-alt mr-1" />{{ t('sessions.chart_throughput') }}
        </span>
        <span class="chart-subtitle">{{ protocol === 'rtsp' ? 'Mbps' : 'packets/s' }}</span>
      </div>
      <div class="chart-wrapper">
        <Line :data="throughputChartData" :options="baseChartOptions" />
      </div>
    </div>

    <!-- Quality Chart -->
    <div class="chart-container">
      <div class="chart-header">
        <span class="chart-title">
          <i class="fas fa-exclamation-triangle mr-1" />{{ t('sessions.chart_quality') }}
        </span>
        <span class="chart-subtitle">{{ t('sessions.chart_events') }}</span>
      </div>
      <div class="chart-wrapper">
        <Line :data="qualityChartData" :options="qualityChartOptions" />
      </div>
    </div>

    <!-- Frame Rate Chart -->
    <div class="chart-container">
      <div class="chart-header">
        <span class="chart-title">
          <i class="fas fa-film mr-1" />{{ t('sessions.chart_framerate') }}
        </span>
        <span class="chart-subtitle">{{ protocol === 'rtsp' ? 'FPS' : 'packets/s' }}</span>
      </div>
      <div class="chart-wrapper">
        <Line :data="fpsChartData" :options="fpsChartOptions" />
      </div>
    </div>
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
import { Line } from 'vue-chartjs';
import type { SessionSample } from '@/types/sessions';

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, Filler, Tooltip, Legend);

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
}

const props = defineProps<{
  session?: SessionSnapshot;
  sessionId?: string;
  protocol?: 'rtsp' | 'webrtc';
  mode?: 'live' | 'history';
  historyData?: SessionSample[];
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
    };
  });
}

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
          // WebRTC: packet-based throughput, drop-based quality
          const deltaVideoPackets =
            (session.video_packets ?? 0) - (prevSnapshot.video_packets ?? 0);
          throughput_mbps = Math.max(0, deltaVideoPackets / dt);
          delta_losses = Math.max(
            0,
            (session.video_dropped ?? 0) - (prevSnapshot.video_dropped ?? 0),
          );
          delta_idr = Math.max(0, (session.audio_dropped ?? 0) - (prevSnapshot.audio_dropped ?? 0));
          const deltaAudioPackets =
            (session.audio_packets ?? 0) - (prevSnapshot.audio_packets ?? 0);
          actual_fps = Math.max(0, deltaAudioPackets / dt);
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

// Shared styling
const gridColor = 'rgba(128, 128, 128, 0.15)';
const tickColor = 'rgba(128, 128, 128, 0.6)';

const baseChartOptions = computed(() => ({
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
}));

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
      label:
        props.protocol !== 'webrtc' ? t('sessions.chart_framerate') : t('sessions.audio_packets'),
      data: displayData.value.map((p) => p.actual_fps),
      borderColor: 'rgb(236, 72, 153)',
      backgroundColor: 'rgba(236, 72, 153, 0.1)',
      fill: true,
    },
  ],
}));
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
.chart-subtitle {
  @apply text-[10px] uppercase tracking-wider opacity-50 font-semibold;
}
.chart-wrapper {
  height: 160px;
}
</style>
