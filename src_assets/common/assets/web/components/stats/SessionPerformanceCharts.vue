<script setup lang="ts">
import { computed } from 'vue';
import { useI18n } from 'vue-i18n';

import MetricChart from './MetricChart.vue';
import type { PerformancePoint } from './types';

const props = withDefaults(
  defineProps<{
    points: PerformancePoint[];
    protocol?: string;
    targetFps?: number;
  }>(),
  {
    protocol: 'rtsp',
    targetFps: 0,
  },
);

const { t } = useI18n();
const latest = computed(() => props.points.at(-1));
const latency = computed(() =>
  props.points.flatMap((point) => (point.latencyMs == null ? [] : [point.latencyMs])),
);
const throughput = computed(() => props.points.map((point) => point.throughputMbps));
const quality = computed(() => props.points.map((point) => point.qualityEvents));
const frameRate = computed(() => props.points.map((point) => point.fps));
const qualityLabel = computed(() =>
  props.protocol.toLocaleLowerCase() === 'webrtc'
    ? t('sessions.video_dropped')
    : t('sessions.chart_quality'),
);

function decimal(value: number | null | undefined, suffix: string, digits = 1): string {
  if (value == null || !Number.isFinite(value)) return '—';
  return `${value.toLocaleString(undefined, { maximumFractionDigits: digits })}${suffix}`;
}
</script>

<template>
  <div class="performance-charts">
    <MetricChart
      :title="t('sessions.chart_encode_latency')"
      :description="t('sessions.tip_chart_encode_latency')"
      :value="decimal(latest?.latencyMs, ' ms')"
      :values="latency"
      unit=" ms"
      color="var(--vs-color-status-info)"
      :target="16"
    />
    <MetricChart
      :title="t('sessions.chart_throughput')"
      :description="t('sessions.tip_chart_throughput')"
      :value="decimal(latest?.throughputMbps, ' Mbps', 2)"
      :values="throughput"
      unit=" Mbps"
      color="var(--vs-color-status-success)"
    />
    <MetricChart
      :title="t('sessions.chart_framerate')"
      :description="t('sessions.tip_chart_framerate')"
      :value="decimal(latest?.fps, ' fps')"
      :values="frameRate"
      unit=" fps"
      color="var(--vs-color-data-accent)"
      :target="targetFps || undefined"
    />
    <MetricChart
      :title="qualityLabel"
      :description="t('sessions.tip_chart_quality')"
      :value="decimal(latest?.qualityEvents, '', 0)"
      :values="quality"
      color="var(--vs-color-status-warning)"
    />
  </div>
</template>

<style scoped>
.performance-charts {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-12);
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
}
</style>
