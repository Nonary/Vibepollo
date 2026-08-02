<template>
  <n-card :segmented="{ content: true }" class="host-stats-history-card">
    <template #header>
      <div class="flex items-center gap-3">
        <i class="fas fa-chart-area text-primary" />
        <span class="text-base font-semibold tracking-tight">{{ $t('stats.history_title') }}</span>
      </div>
    </template>

    <div v-if="history.length < 2" class="text-xs opacity-60">
      {{ $t('stats.history_empty') }}
    </div>

    <div v-else class="space-y-4">
      <section v-for="chart in charts" :key="chart.id" class="history-chart">
        <div class="history-chart__header">
          <span>{{ chart.title }}</span>
          <span class="history-chart__latest">{{ chart.latest }}</span>
        </div>
        <svg class="history-chart__svg" viewBox="0 0 100 40" preserveAspectRatio="none">
          <polyline
            v-for="line in chart.lines"
            :key="line.label"
            :points="line.points"
            :stroke="line.color"
            fill="none"
            stroke-width="1.8"
            vector-effect="non-scaling-stroke"
          />
        </svg>
        <div class="history-chart__legend">
          <span
            v-for="line in chart.lines"
            :key="line.label"
            class="inline-flex items-center gap-1"
          >
            <span class="history-chart__swatch" :style="{ backgroundColor: line.color }" />
            {{ line.label }}
          </span>
        </div>
      </section>
    </div>
  </n-card>
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { storeToRefs } from 'pinia';
import { NCard } from 'naive-ui';
import { useI18n } from 'vue-i18n';
import { useHostStatsStore } from '@/stores/hostStats';
import type { HostHistoryPoint } from '@/types/host';

type MetricLine = {
  label: string;
  color: string;
  points: string;
};

const MAX_RENDERED_POINTS = 240;

const store = useHostStatsStore();
const { history } = storeToRefs(store);
const { t } = useI18n();

const pct = (value: number | undefined) =>
  typeof value === 'number' && Number.isFinite(value) && value >= 0 ? Math.min(100, value) : 0;

const toPoints = (points: HostHistoryPoint[], pick: (point: HostHistoryPoint) => number) => {
  if (points.length < 2) return '';
  const maxIndex = points.length - 1;
  return points
    .map((point, index) => {
      const x = (index / maxIndex) * 100;
      const y = 40 - (Math.min(100, Math.max(0, pick(point))) / 100) * 36 - 2;
      return `${x.toFixed(2)},${y.toFixed(2)}`;
    })
    .join(' ');
};

const downsample = (points: HostHistoryPoint[]) => {
  if (points.length <= MAX_RENDERED_POINTS) return points;
  const lastIndex = points.length - 1;
  return Array.from({ length: MAX_RENDERED_POINTS }, (_, index) => {
    const sourceIndex = Math.round((index / (MAX_RENDERED_POINTS - 1)) * lastIndex);
    return points[sourceIndex];
  });
};

const latestPoint = computed(() => history.value[history.value.length - 1]);

const charts = computed(() => {
  const points = downsample(history.value);
  const latest = latestPoint.value;
  const percentLabel = (...values: Array<number | undefined>) =>
    values.map((value) => `${Math.round(pct(value))}%`).join(' / ');

  return [
    {
      id: 'compute',
      title: t('sessions.chart_host_compute'),
      latest: latest
        ? percentLabel(latest.cpu_percent, latest.gpu_percent, latest.gpu_encoder_percent)
        : '--',
      lines: [
        {
          label: t('sessions.chart_host_cpu'),
          color: '#2563eb',
          points: toPoints(points, (p) => pct(p.cpu_percent)),
        },
        {
          label: t('sessions.chart_host_gpu'),
          color: '#16a34a',
          points: toPoints(points, (p) => pct(p.gpu_percent)),
        },
        {
          label: t('sessions.chart_host_gpu_encoder'),
          color: '#dc2626',
          points: toPoints(points, (p) => pct(p.gpu_encoder_percent)),
        },
      ] satisfies MetricLine[],
    },
    {
      id: 'memory',
      title: t('sessions.chart_host_memory'),
      latest: latest ? percentLabel(latest.ram_percent, latest.vram_percent) : '--',
      lines: [
        {
          label: t('sessions.chart_host_ram'),
          color: '#9333ea',
          points: toPoints(points, (p) => pct(p.ram_percent)),
        },
        {
          label: t('sessions.chart_host_vram'),
          color: '#ea580c',
          points: toPoints(points, (p) => pct(p.vram_percent)),
        },
      ] satisfies MetricLine[],
    },
  ];
});
</script>

<style scoped>
.history-chart {
  border: 1px solid rgba(148, 163, 184, 0.25);
  border-radius: 8px;
  padding: 12px;
}

.history-chart__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  font-size: 12px;
  font-weight: 600;
}

.history-chart__latest {
  font-variant-numeric: tabular-nums;
  opacity: 0.7;
}

.history-chart__svg {
  margin-top: 8px;
  height: 140px;
  width: 100%;
  border-radius: 6px;
  background:
    linear-gradient(to bottom, rgba(148, 163, 184, 0.12) 1px, transparent 1px) 0 0 / 100% 25%,
    rgba(148, 163, 184, 0.08);
}

.history-chart__legend {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  margin-top: 8px;
  font-size: 11px;
  opacity: 0.8;
}

.history-chart__swatch {
  display: inline-block;
  width: 8px;
  height: 8px;
  border-radius: 999px;
}
</style>
