<script setup lang="ts">
import { configBoolean } from '@/utils/settings';
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';

import { ApiError, apiDelete, apiGet, apiPost } from '@/api/client';
import {
  AppButton,
  ConfirmDialog,
  EmptyState,
  InlineAlert,
  LoadingSkeleton,
  PageHeader,
  StatusBadge,
} from '@/components/ui';
import MetricChart from '@/components/stats/MetricChart.vue';
import MetricGauge from '@/components/stats/MetricGauge.vue';
import SessionDetailDialog from '@/components/stats/SessionDetailDialog.vue';
import SessionPerformanceCharts from '@/components/stats/SessionPerformanceCharts.vue';
import HostComputeChart from '@/components/stats/HostComputeChart.vue';
import {
  groupSessionSummaries,
  parseHistoryPage,
  type SessionHistoryRow,
} from '@/components/stats/historyUtils';
import type { ChartValuePoint, PerformancePoint } from '@/components/stats/types';
import type { HostInfo, HostStatsSnapshot } from '@/types/host';
import type { RTSPSession, SessionSummary, WebRTCSession } from '@/types/sessions';
import { formatBitrate, formatBytes, formatDuration, formatRelativeTime } from '@/utils/format';
import { downsampleHostHistory } from '@/utils/v2Parity';

const { locale, t } = useI18n();

interface ConfigResponse extends Record<string, unknown> {
  status?: boolean;
}

interface SessionsPayload<T> {
  sessions?: T[];
}

interface MutationResponse {
  status?: boolean | string;
  error?: string;
}

interface CounterSnapshot {
  timestamp: number;
  bytes: number;
  frames: number;
  losses: number;
  recovery: number;
}

interface TimedHostStats extends HostStatsSnapshot {
  timestamp: number;
}

interface ActiveVisualSession {
  key: string;
  id: string;
  protocol: 'rtsp' | 'webrtc';
  client: string;
  codec: string;
  resolution: string;
  targetFps: number;
  bitrateKbps: number;
  duration: string;
  latencyMs: number | null;
  points: PerformancePoint[];
  hdr: boolean;
}

const config = ref<ConfigResponse>({});
const hostStats = ref<HostStatsSnapshot | null>(null);
const hostInfo = ref<HostInfo | null>(null);
const hostHistory = ref<TimedHostStats[]>([]);
const rtspSessions = ref<RTSPSession[]>([]);
const webRtcSessions = ref<WebRTCSession[]>([]);
const histories = ref<Record<string, PerformancePoint[]>>({});
const counterSnapshots = new Map<string, CounterSnapshot>();
const sessionHistory = ref<SessionSummary[]>([]);
const selectedHistory = ref<SessionSummary | null>(null);
const selectedHistoryMembers = ref<SessionSummary[]>([]);
const historyPage = ref(1);
const historyTotal = ref(0);
const historyHasMore = ref(false);
const historyLoading = ref(false);
const historyError = ref('');
const detailOpen = ref(false);
const stopConfirmOpen = ref(false);
const pendingStop = ref<ActiveVisualSession | null>(null);
const ready = ref(false);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const lastUpdated = ref<number | null>(null);
const stoppingSessionKey = ref('');
let refreshTimer: number | undefined;
let refreshInFlight = false;
let historyRequestGeneration = 0;
let disposed = false;

const HISTORY_PAGE_SIZE = 12;

const statsEnabled = computed(() => configBoolean(config.value.realtime_stats_enabled, true));
const showActiveSessions = computed(() =>
  configBoolean(config.value.realtime_stats_show_active_sessions, true),
);
const showHostStats = computed(() =>
  configBoolean(config.value.realtime_stats_show_host_stats, true),
);
const showHostCharts = computed(() =>
  configBoolean(config.value.realtime_stats_show_host_charts, true),
);
const showSessionHistory = computed(() =>
  configBoolean(config.value.realtime_stats_show_session_history, true),
);
const pollInterval = computed(() => {
  const configured = Number(config.value.realtime_stats_poll_interval_ms ?? 2000);
  return Math.max(1000, Math.min(10000, Number.isFinite(configured) ? configured : 2000));
});
const pauseWhenHidden = computed(() =>
  configBoolean(config.value.realtime_stats_pause_when_hidden, true),
);
const maxHistoryPoints = computed(() => {
  const configured = Number(config.value.realtime_stats_max_history_points ?? 300);
  return Math.max(30, Math.min(2000, Number.isFinite(configured) ? configured : 300));
});
const retentionMs = computed(() => {
  const configured = Number(config.value.realtime_stats_history_retention_seconds ?? 300);
  return Math.max(60, Number.isFinite(configured) ? configured : 300) * 1000;
});

const activeSessions = computed<ActiveVisualSession[]>(() => [
  ...rtspSessions.value.map((session) => ({
    key: `rtsp:${session.uuid}`,
    id: session.uuid,
    protocol: 'rtsp' as const,
    client: session.device_name || t('ui.sessions.value.moonlight_client'),
    codec: session.codec || t('ui.sessions.value.unknown_codec'),
    resolution: `${session.width} × ${session.height}`,
    targetFps: session.fps,
    bitrateKbps: session.encoder_bitrate_kbps,
    duration: formatDuration(session.uptime_seconds, locale.value),
    latencyMs:
      Number.isFinite(session.encode_latency_ms) && session.encode_latency_ms >= 0
        ? session.encode_latency_ms
        : null,
    points: histories.value[`rtsp:${session.uuid}`] ?? [],
    hdr: session.hdr,
  })),
  ...webRtcSessions.value.map((session) => ({
    key: `webrtc:${session.id}`,
    id: session.id,
    protocol: 'webrtc' as const,
    client: t('ui.sessions.value.browser_session', { id: session.id.slice(0, 8) }),
    codec: session.codec || t('ui.sessions.value.negotiating_codec'),
    resolution:
      session.width && session.height
        ? `${session.width} × ${session.height}`
        : t('ui.sessions.value.not_reported'),
    targetFps: session.fps ?? 0,
    bitrateKbps: session.encoder_bitrate_kbps ?? 0,
    duration: t('sessions.live'),
    latencyMs: null,
    points: histories.value[`webrtc:${session.id}`] ?? [],
    hdr: session.hdr ?? false,
  })),
]);

const activeRtspSessionCount = computed(
  () => activeSessions.value.filter((session) => session.protocol === 'rtsp').length,
);
const stopConfirmTitle = computed(() =>
  pendingStop.value?.protocol === 'rtsp'
    ? t('ui.sessions.confirm.stop_rtsp_title')
    : t('ui.sessions.confirm.stop_webrtc_title'),
);
const stopConfirmDescription = computed(() => {
  if (pendingStop.value?.protocol !== 'rtsp')
    return t('ui.sessions.confirm.stop_webrtc_description');
  return activeRtspSessionCount.value > 1
    ? t('ui.sessions.confirm.stop_rtsp_all_description')
    : t('ui.sessions.confirm.stop_rtsp_description');
});

const networkTxPoints = computed<ChartValuePoint[]>(() =>
  hostHistory.value.map((point) => ({
    timestamp: point.timestamp,
    value:
      typeof point.net_tx_bps === 'number' && point.net_tx_bps >= 0
        ? point.net_tx_bps / 1_000_000
        : null,
  })),
);
const networkRxPoints = computed<ChartValuePoint[]>(() =>
  hostHistory.value.map((point) => ({
    timestamp: point.timestamp,
    value:
      typeof point.net_rx_bps === 'number' && point.net_rx_bps >= 0
        ? point.net_rx_bps / 1_000_000
        : null,
  })),
);
const comparableHostHistory = computed(() => downsampleHostHistory(hostHistory.value));
const historyRows = computed<SessionHistoryRow[]>(() =>
  groupSessionSummaries(sessionHistory.value),
);
const historyPageCount = computed(() =>
  historyTotal.value > 0
    ? Math.max(1, Math.ceil(historyTotal.value / HISTORY_PAGE_SIZE))
    : historyHasMore.value
      ? historyPage.value + 1
      : 1,
);
const historyCanNext = computed(
  () => historyHasMore.value || historyPage.value < historyPageCount.value,
);
const historyCountLabel = computed(() => {
  const count = historyTotal.value || sessionHistory.value.length;
  return historyHasMore.value ? `${count}+` : String(count);
});
const hostCurrent = computed(() => ({
  cpu: hostStats.value?.cpu_percent ?? null,
  gpu: hostStats.value?.gpu_percent ?? null,
  encoder: hostStats.value?.gpu_encoder_percent ?? null,
}));

watch(statsEnabled, (enabled) => {
  if (enabled) return;
  // A disabled live dashboard must not present the last pre-pause sample as
  // current, and its counter baseline must not span the disabled interval.
  histories.value = {};
  hostHistory.value = [];
  hostStats.value = null;
  hostInfo.value = null;
  counterSnapshots.clear();
});

function percent(value: number | undefined): string {
  return Number.isFinite(value) ? `${Math.round(value ?? 0)}%` : '—';
}

function temperature(value: number | undefined): string {
  return Number.isFinite(value) && (value ?? 0) > 0
    ? `${Math.round(value ?? 0)} °C`
    : t('ui.sessions.value.not_reported');
}

function appendPerformance(
  key: string,
  current: CounterSnapshot,
  latencyMs: number | null,
  fallbackFps: number,
  fallbackMbps: number,
  dropped?: { video: number; audio: number },
): void {
  const previous = counterSnapshots.get(key);
  let fps = fallbackFps;
  let throughputMbps = fallbackMbps;
  let qualityEvents = 0;
  let videoDropped = 0;
  let audioDropped = 0;
  if (previous) {
    const seconds = (current.timestamp - previous.timestamp) / 1000;
    if (seconds > 0) {
      fps = Math.max(0, (current.frames - previous.frames) / seconds);
      throughputMbps = Math.max(0, ((current.bytes - previous.bytes) * 8) / seconds / 1_000_000);
      qualityEvents =
        Math.max(0, current.losses - previous.losses) +
        Math.max(0, current.recovery - previous.recovery);
    }
    if (dropped) {
      videoDropped = Math.max(0, dropped.video - previous.losses);
      audioDropped = Math.max(0, dropped.audio - previous.recovery);
    }
  }
  counterSnapshots.set(key, current);
  const point: PerformancePoint = {
    timestamp: current.timestamp,
    latencyMs,
    throughputMbps: Math.round(throughputMbps * 100) / 100,
    qualityEvents,
    fps: Math.round(fps * 10) / 10,
    ...(dropped ? { videoDropped, audioDropped } : {}),
  };
  const cutoff = current.timestamp - retentionMs.value;
  histories.value = {
    ...histories.value,
    [key]: [...(histories.value[key] ?? []), point]
      .filter((candidate) => candidate.timestamp >= cutoff)
      .slice(-maxHistoryPoints.value),
  };
}

function recordSessionSamples(now: number): void {
  if (!statsEnabled.value) {
    // A disabled sampler must not leave a stale baseline that turns the first
    // post-pause sample into an artificial rate spike.
    counterSnapshots.clear();
    return;
  }
  for (const session of rtspSessions.value) {
    appendPerformance(
      `rtsp:${session.uuid}`,
      {
        timestamp: now,
        bytes: session.bytes_sent,
        frames: session.frames_sent,
        losses: session.client_reported_losses,
        recovery: session.idr_requests + session.invalidate_ref_count,
      },
      Number.isFinite(session.encode_latency_ms) && session.encode_latency_ms >= 0
        ? session.encode_latency_ms
        : null,
      session.fps,
      session.encoder_bitrate_kbps / 1000,
    );
  }
  for (const session of webRtcSessions.value) {
    appendPerformance(
      `webrtc:${session.id}`,
      {
        timestamp: now,
        bytes: session.bytes_sent,
        frames: session.last_video_frame_index,
        losses: session.video_dropped,
        recovery: session.audio_dropped,
      },
      null,
      session.fps ?? 0,
      (session.encoder_bitrate_kbps ?? 0) / 1000,
      { video: session.video_dropped, audio: session.audio_dropped },
    );
  }
}

function scheduleRefresh(): void {
  if (refreshTimer !== undefined) window.clearTimeout(refreshTimer);
  refreshTimer = window.setTimeout(async () => {
    if (disposed) return;
    if (!document.hidden || !pauseWhenHidden.value) await refresh(true);
    if (disposed) return;
    scheduleRefresh();
  }, pollInterval.value);
}

async function loadConfig(): Promise<void> {
  try {
    config.value = await apiGet<ConfigResponse>('/api/config');
  } catch {
    config.value = {};
  }
}

async function loadHistoryPage(page: number, silent = false): Promise<void> {
  const nextPage = Math.max(1, Math.floor(page));
  const generation = ++historyRequestGeneration;
  if (!silent) historyLoading.value = true;
  try {
    const offset = (nextPage - 1) * HISTORY_PAGE_SIZE;
    const payload = await apiGet<SessionsPayload<SessionSummary> & Record<string, unknown>>(
      `/api/history/sessions?limit=${HISTORY_PAGE_SIZE + 1}&offset=${offset}`,
    );
    if (generation !== historyRequestGeneration || disposed) return;
    const parsed = parseHistoryPage(payload, HISTORY_PAGE_SIZE);
    if (!parsed.sessions.length && nextPage > 1) {
      await loadHistoryPage(nextPage - 1, silent);
      return;
    }
    sessionHistory.value = parsed.sessions;
    historyPage.value = nextPage;
    if (parsed.total != null) {
      historyTotal.value = Math.max(0, parsed.total);
    } else if (parsed.hasMore) {
      historyTotal.value = Math.max(historyTotal.value, offset + HISTORY_PAGE_SIZE + 1);
    } else {
      historyTotal.value = offset + parsed.sessions.length;
    }
    historyHasMore.value =
      parsed.hasMore && (parsed.total == null || offset + HISTORY_PAGE_SIZE < historyTotal.value);
    historyError.value = '';
  } catch {
    if (generation === historyRequestGeneration) {
      historyError.value = t('ui.sessions.alert.partial_failure_description');
    }
  } finally {
    if (generation === historyRequestGeneration) historyLoading.value = false;
  }
}

function openHistory(row: SessionHistoryRow): void {
  selectedHistory.value = row.summary;
  selectedHistoryMembers.value = row.members;
  detailOpen.value = true;
}

async function changeHistoryPage(page: number): Promise<void> {
  if (page < 1 || page > historyPageCount.value || page === historyPage.value) return;
  await loadHistoryPage(page);
}

async function onHistoryDeleted(uuid: string): Promise<void> {
  selectedHistoryMembers.value = [];
  sessionHistory.value = sessionHistory.value.filter((session) => session.uuid !== uuid);
  historyTotal.value = Math.max(0, historyTotal.value - 1);
  const page =
    historyPage.value > 1 && sessionHistory.value.length === 0
      ? historyPage.value - 1
      : historyPage.value;
  await loadHistoryPage(page, true);
}

async function refresh(silent = false): Promise<void> {
  if (refreshInFlight) return;
  refreshInFlight = true;
  if (!silent) refreshing.value = true;

  const requests: Promise<unknown>[] = [
    apiGet<SessionsPayload<RTSPSession>>('/api/rtsp/sessions'),
    apiGet<SessionsPayload<WebRTCSession>>('/api/webrtc/sessions'),
  ];
  if (statsEnabled.value) {
    requests.push(apiGet<HostStatsSnapshot>('/api/host/stats'), apiGet<HostInfo>('/api/host/info'));
  }

  const historyRequest = loadHistoryPage(historyPage.value, silent || ready.value);
  const results = await Promise.allSettled(requests);
  await historyRequest;
  if (disposed) {
    refreshing.value = false;
    refreshInFlight = false;
    return;
  }
  const rtspResult = results[0] as PromiseSettledResult<SessionsPayload<RTSPSession>>;
  const webRtcResult = results[1] as PromiseSettledResult<SessionsPayload<WebRTCSession>>;
  if (rtspResult.status === 'fulfilled') rtspSessions.value = rtspResult.value.sessions ?? [];
  if (webRtcResult.status === 'fulfilled') webRtcSessions.value = webRtcResult.value.sessions ?? [];

  if (statsEnabled.value) {
    const statsResult = results[2] as PromiseSettledResult<HostStatsSnapshot>;
    const infoResult = results[3] as PromiseSettledResult<HostInfo>;
    if (statsResult.status === 'fulfilled') {
      hostStats.value = statsResult.value;
      const timestamp = Date.now();
      const cutoff = timestamp - retentionMs.value;
      hostHistory.value = [...hostHistory.value, { ...statsResult.value, timestamp }]
        .filter((candidate) => candidate.timestamp >= cutoff)
        .slice(-maxHistoryPoints.value);
    }
    if (infoResult.status === 'fulfilled') hostInfo.value = infoResult.value;
  }

  const now = Date.now();
  if (rtspResult.status === 'fulfilled' || webRtcResult.status === 'fulfilled')
    recordSessionSamples(now);
  const failures = results.filter((result) => result.status === 'rejected').length;
  error.value = failures ? t('ui.sessions.alert.partial_failure_description') : '';
  if (results.some((result) => result.status === 'fulfilled')) lastUpdated.value = now;
  ready.value = true;
  refreshing.value = false;
  refreshInFlight = false;
}

function requestStop(session: ActiveVisualSession): void {
  pendingStop.value = session;
  stopConfirmOpen.value = true;
}

function clearPendingStop(): void {
  if (!stoppingSessionKey.value) pendingStop.value = null;
}

async function confirmStop(): Promise<void> {
  const session = pendingStop.value;
  if (!session) return;

  error.value = '';
  notice.value = '';
  stoppingSessionKey.value = session.key;
  try {
    if (session.protocol === 'rtsp') {
      const response = await apiPost<MutationResponse>('/api/apps/close', {});
      if (response.status !== true) throw new Error(t('ui.sessions.error.stop_rtsp_rejected'));
      notice.value = t('ui.sessions.notice.stop_rtsp');
    } else {
      const response = await apiDelete<MutationResponse>(
        `/api/webrtc/sessions/${encodeURIComponent(session.id)}`,
      );
      if (response.status !== true) {
        throw new Error(response.error || t('ui.sessions.error.webrtc_not_found'));
      }
      notice.value = t('ui.sessions.notice.stop_webrtc');
    }

    stopConfirmOpen.value = false;
    pendingStop.value = null;
    await refresh(true);
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.sessions.error.action')
        : cause instanceof Error
          ? cause.message
          : t('ui.sessions.error.action');
  } finally {
    stoppingSessionKey.value = '';
  }
}

function historyDate(history: SessionSummary): string {
  const timestamp = history.end_time_unix || history.start_time_unix;
  return formatRelativeTime(timestamp * 1000, locale.value, t('ui.sessions.value.unknown_time'));
}

onMounted(async () => {
  disposed = false;
  await loadConfig();
  if (disposed) return;
  await refresh();
  if (disposed) return;
  scheduleRefresh();
});

onBeforeUnmount(() => {
  disposed = true;
  historyRequestGeneration += 1;
  if (refreshTimer !== undefined) window.clearTimeout(refreshTimer);
});
</script>

<template>
  <div class="vs-page vs-page--dashboard stats-page">
    <PageHeader :title="t('stats.title')" :description="t('stats.subtitle')">
      <template #meta>
        <StatusBadge
          :label="statsEnabled ? t('stats.live') : t('stats.paused')"
          :tone="statsEnabled ? 'success' : 'neutral'"
          compact
        />
        <StatusBadge
          :label="
            t('ui.sessions.count.active', { count: activeSessions.length }, activeSessions.length)
          "
          :tone="activeSessions.length ? 'info' : 'neutral'"
          compact
        />
        <span v-if="lastUpdated" class="stats-updated">
          {{
            t('clients.last_updated', {
              time: formatRelativeTime(lastUpdated, locale, t('ui.sessions.value.unknown_time')),
            })
          }}
        </span>
      </template>
      <template #actions>
        <RouterLink class="vs-button vs-button--secondary vs-button--default" to="/settings">
          {{ t('stats.settings') }}
        </RouterLink>
        <AppButton
          :label="t('_common.refresh')"
          icon="refresh"
          :busy="refreshing"
          :busy-label="t('ui.sessions.action.refreshing')"
          @click="refresh()"
        />
      </template>
    </PageHeader>

    <div class="stats-stack">
      <InlineAlert
        v-if="error"
        tone="warning"
        :title="t('ui.sessions.alert.partial_failure_title')"
      >
        {{ error }}
      </InlineAlert>
      <InlineAlert
        v-if="notice"
        tone="success"
        :title="t('ui.sessions.alert.action_complete')"
        announce="polite"
        :dismiss-label="t('_common.dismiss')"
        @dismiss="notice = ''"
      >
        {{ notice }}
      </InlineAlert>
      <InlineAlert v-if="!statsEnabled" tone="info" :title="t('stats.disabled_title')">
        {{ t('stats.disabled_desc') }}
      </InlineAlert>

      <section
        v-if="statsEnabled && showHostStats"
        class="stats-section"
        aria-labelledby="host-vitals-title"
      >
        <div class="stats-section__heading">
          <div>
            <h2 id="host-vitals-title">{{ t('ui.stats.host_vitals_title') }}</h2>
            <p>
              {{ hostInfo?.cpu_model || ''
              }}<span v-if="hostInfo?.gpu_model"> · {{ hostInfo.gpu_model }}</span>
            </p>
          </div>
          <span v-if="hostStats"
            >{{ temperature(hostStats.cpu_temp_c) }} CPU ·
            {{ temperature(hostStats.gpu_temp_c) }} GPU</span
          >
        </div>

        <div v-if="!ready" class="gauge-grid">
          <LoadingSkeleton v-for="item in 4" :key="item" variant="block" height="12rem" />
        </div>
        <div v-else-if="hostStats" class="gauge-grid">
          <MetricGauge
            :label="t('ui.stats.host_cpu')"
            :value="hostStats.cpu_percent"
            :detail="temperature(hostStats.cpu_temp_c)"
            color="var(--vs-color-status-info)"
          />
          <MetricGauge
            :label="t('ui.stats.host_gpu')"
            :value="hostStats.gpu_percent"
            :detail="temperature(hostStats.gpu_temp_c)"
            color="var(--vs-color-status-success)"
          />
          <MetricGauge
            :label="t('ui.stats.host_ram')"
            :value="hostStats.ram_percent"
            :detail="`${formatBytes(hostStats.ram_used_bytes, locale)} / ${formatBytes(hostStats.ram_total_bytes, locale)}`"
            color="var(--vs-color-status-warning)"
          />
          <MetricGauge
            :label="t('ui.stats.host_vram')"
            :value="hostStats.vram_percent"
            :detail="`${formatBytes(hostStats.vram_used_bytes, locale)} / ${formatBytes(hostStats.vram_total_bytes, locale)}`"
            color="var(--vs-color-data-accent)"
          />
        </div>
      </section>

      <section
        v-if="statsEnabled && showHostCharts"
        class="stats-section"
        aria-labelledby="host-history-title"
      >
        <div class="stats-section__heading">
          <div>
            <h2 id="host-history-title">{{ t('stats.history_title') }}</h2>
            <p>{{ t('sessions.tip_chart_host_compute') }}</p>
          </div>
          <span>{{
            t('ui.stats.sample_count', { count: hostHistory.length }, hostHistory.length)
          }}</span>
        </div>
        <div v-if="hostHistory.length" class="host-chart-grid">
          <HostComputeChart
            :title="t('sessions.chart_host_cpu')"
            :points="comparableHostHistory"
            :current="hostCurrent"
          />
          <MetricChart
            :title="t('sessions.chart_host_net_rx')"
            :value="
              hostStats?.net_rx_bps == null
                ? '—'
                : `${(hostStats.net_rx_bps / 1_000_000).toFixed(2)} Mbps`
            "
            :points="networkRxPoints"
            unit=" Mbps"
            color="var(--vs-color-status-success)"
          />
          <MetricChart
            :title="t('sessions.chart_host_net_tx')"
            :value="
              hostStats?.net_tx_bps == null
                ? '—'
                : `${(hostStats.net_tx_bps / 1_000_000).toFixed(2)} Mbps`
            "
            :points="networkTxPoints"
            unit=" Mbps"
            color="var(--vs-color-status-info)"
          />
        </div>
        <EmptyState v-else :title="t('stats.history_empty')" icon="activity" compact />
      </section>

      <section
        v-if="showActiveSessions"
        class="stats-section"
        aria-labelledby="stream-performance-title"
      >
        <div class="stats-section__heading">
          <div>
            <h2 id="stream-performance-title">{{ t('sessions.title') }}</h2>
            <p>{{ t('sessions.active_unified') }}</p>
          </div>
          <span>{{ activeSessions.length }}</span>
        </div>

        <EmptyState
          v-if="ready && !activeSessions.length"
          :title="t('sessions.no_active')"
          :description="t('ui.sessions.active.empty_description')"
          icon="sessions"
          compact
        />
        <div v-else class="visual-session-list">
          <article
            v-for="session in activeSessions"
            :key="session.key"
            class="visual-session vs-surface"
          >
            <header class="visual-session__header">
              <div>
                <div class="visual-session__badges">
                  <StatusBadge :label="t('sessions.live')" tone="success" compact />
                  <StatusBadge :label="session.protocol.toUpperCase()" tone="info" compact />
                  <StatusBadge
                    v-if="session.hdr"
                    :label="t('sessions.history_hdr')"
                    tone="warning"
                    compact
                  />
                </div>
                <h3>{{ session.client }}</h3>
                <p>
                  {{ session.resolution }} @ {{ session.targetFps || '—' }} · {{ session.codec }}
                </p>
              </div>
              <dl>
                <div>
                  <dt>{{ t('sessions.bitrate') }}</dt>
                  <dd>{{ formatBitrate(session.bitrateKbps, locale) }}</dd>
                </div>
                <div>
                  <dt>{{ t('sessions.encode_latency') }}</dt>
                  <dd>
                    {{ session.latencyMs == null ? '—' : `${session.latencyMs.toFixed(1)} ms` }}
                  </dd>
                </div>
                <div>
                  <dt>{{ t('sessions.uptime') }}</dt>
                  <dd>{{ session.duration }}</dd>
                </div>
              </dl>
              <AppButton
                :label="t('ui.sessions.action.stop_stream')"
                icon="stop"
                variant="secondary"
                class="visual-session__stop"
                :disabled="Boolean(stoppingSessionKey)"
                :aria-label="
                  t('ui.sessions.action.stop_stream_named', {
                    protocol:
                      session.protocol === 'rtsp'
                        ? t('ui.sessions.protocol.rtsp')
                        : t('ui.sessions.protocol.webrtc'),
                    client: session.client,
                  })
                "
                @click="requestStop(session)"
              />
            </header>
            <SessionPerformanceCharts
              :points="session.points"
              :protocol="session.protocol"
              :target-fps="session.targetFps"
              :session-id="session.id"
              mode="live"
              :live-enabled="statsEnabled"
              :pause-when-hidden="pauseWhenHidden"
            />
          </article>
        </div>
      </section>

      <section
        v-if="showSessionHistory"
        class="stats-section"
        aria-labelledby="visual-history-title"
      >
        <div class="stats-section__heading">
          <div>
            <h2 id="visual-history-title">{{ t('sessions.history_title') }}</h2>
            <p>{{ t('ui.sessions.history.description') }}</p>
          </div>
          <span>{{ historyCountLabel }}</span>
        </div>
        <InlineAlert
          v-if="historyError"
          tone="warning"
          :title="t('ui.sessions.alert.partial_failure_title')"
        >
          {{ historyError }}
        </InlineAlert>
        <EmptyState
          v-if="ready && !historyLoading && !sessionHistory.length"
          :title="t('sessions.history_empty')"
          icon="logs"
          compact
        />
        <div v-else-if="historyLoading && !sessionHistory.length" class="history-loading">
          <LoadingSkeleton v-for="item in 3" :key="item" variant="block" height="8rem" />
        </div>
        <div v-else class="history-card-grid">
          <button
            v-for="history in historyRows"
            :key="history.key"
            class="history-card"
            type="button"
            @click="openHistory(history)"
          >
            <span class="history-card__topline">
              <StatusBadge
                :label="
                  history.isGroup
                    ? t('sessions.history_group_title', { count: history.members.length })
                    : (history.summary.protocol || t('_common.unknown')).toUpperCase()
                "
                tone="info"
                compact
              />
              <time>{{ historyDate(history.summary) }}</time>
            </span>
            <strong>{{ history.summary.app_name || t('ui.sessions.value.desktop_stream') }}</strong>
            <span>{{
              history.summary.client_name ||
              history.summary.device_name ||
              t('ui.sessions.value.unknown_client')
            }}</span>
            <span class="history-card__stream"
              >{{ history.summary.width }} × {{ history.summary.height }} @
              {{ history.summary.target_fps }} · {{ history.summary.codec }}</span
            >
            <span class="history-card__action">{{ t('sessions.history_view_detail') }} →</span>
          </button>
        </div>
        <div
          v-if="sessionHistory.length"
          class="history-pagination"
          role="navigation"
          :aria-label="t('sessions.history_title')"
        >
          <AppButton
            :label="t('ui.sessions.history.previous_page', 'Previous page')"
            icon="chevron-left"
            size="compact"
            variant="secondary"
            :disabled="historyPage <= 1 || historyLoading"
            @click="changeHistoryPage(historyPage - 1)"
          />
          <span aria-live="polite">{{ historyPage }} / {{ historyPageCount }}</span>
          <AppButton
            :label="t('ui.sessions.history.next_page', 'Next page')"
            icon="chevron-right"
            icon-position="end"
            size="compact"
            variant="secondary"
            :disabled="!historyCanNext || historyLoading"
            @click="changeHistoryPage(historyPage + 1)"
          />
        </div>
      </section>
    </div>

    <SessionDetailDialog
      v-model:open="detailOpen"
      :summary="selectedHistory"
      :members="selectedHistoryMembers"
      @deleted="onHistoryDeleted"
    />
    <ConfirmDialog
      v-model:open="stopConfirmOpen"
      :title="stopConfirmTitle"
      :description="stopConfirmDescription"
      :confirm-label="t('ui.sessions.action.stop_stream')"
      :cancel-label="t('_common.cancel')"
      :busy="Boolean(pendingStop && stoppingSessionKey === pendingStop.key)"
      :busy-label="t('ui.sessions.action.working')"
      :close-on-confirm="false"
      @confirm="confirmStop"
      @cancel="clearPendingStop"
    />
  </div>
</template>

<style scoped>
.stats-page,
.stats-stack,
.stats-section,
.visual-session-list {
  display: grid;
}

.stats-page,
.stats-stack {
  gap: var(--vs-space-24);
}

.stats-section,
.visual-session-list {
  gap: var(--vs-space-12);
}

.stats-updated,
.stats-section__heading p,
.stats-section__heading > span {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.stats-section__heading {
  display: flex;
  align-items: end;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.stats-section__heading h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.stats-section__heading p {
  margin-top: var(--vs-space-4);
}

.gauge-grid,
.host-chart-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.visual-session {
  display: grid;
  gap: var(--vs-space-12);
  padding: var(--vs-space-20);
  overflow: hidden;
}

.visual-session__header {
  display: flex;
  align-items: flex-end;
  justify-content: space-between;
  gap: var(--vs-space-20);
}

.visual-session__header h3 {
  margin-top: var(--vs-space-8);
  font-size: var(--vs-type-size-section);
}

.visual-session__header p {
  margin-top: var(--vs-space-2);
  color: var(--vs-color-text-secondary);
}

.visual-session__badges {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
}

.visual-session__header dl {
  display: grid;
  grid-template-columns: repeat(3, auto);
  gap: var(--vs-space-24);
}

.visual-session__stop {
  flex: 0 0 auto;
}

.visual-session__header dt {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-semibold);
  text-transform: uppercase;
}

.visual-session__header dd {
  margin-top: var(--vs-space-2);
  font-variant-numeric: tabular-nums;
}

.history-card-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.history-loading {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: var(--vs-space-12);
}

.history-pagination {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: var(--vs-space-12);
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.history-card {
  display: grid;
  min-width: 0;
  gap: var(--vs-space-4);
  padding: var(--vs-space-16);
  border: 1px solid var(--vs-color-border-subtle);
  border-radius: var(--vs-radius-card);
  background: var(--vs-color-bg-surface);
  color: var(--vs-color-text-secondary);
  text-align: left;
  cursor: pointer;
  transition:
    border-color var(--vs-motion-duration-control) var(--vs-motion-easing-standard),
    transform var(--vs-motion-duration-control) var(--vs-motion-easing-standard);
}

.history-card:hover {
  border-color: var(--vs-color-accent-default);
  transform: translateY(-1px);
}

.history-card:focus-visible {
  outline: var(--vs-focus-width) solid var(--vs-focus-ring);
  outline-offset: 2px;
}

.history-card__topline {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--vs-space-8);
  margin-bottom: var(--vs-space-8);
}

.history-card time,
.history-card__stream,
.history-card__action {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
}

.history-card strong {
  overflow: hidden;
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.history-card__stream {
  margin-top: var(--vs-space-8);
  font-variant-numeric: tabular-nums;
}

.history-card__action {
  margin-top: var(--vs-space-8);
  color: var(--vs-color-accent-default);
  font-weight: var(--vs-type-weight-semibold);
}

@media (max-width: 1199px) {
  .gauge-grid,
  .host-chart-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .history-card-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .history-loading {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .visual-session__header {
    align-items: flex-start;
    flex-direction: column;
  }

  .visual-session__stop {
    align-self: stretch;
  }
}

@media (max-width: 639px) {
  .gauge-grid,
  .host-chart-grid,
  .history-card-grid,
  .history-loading {
    grid-template-columns: minmax(0, 1fr);
  }

  .visual-session {
    padding: var(--vs-space-16);
  }

  .visual-session__header dl {
    width: 100%;
    grid-template-columns: minmax(0, 1fr);
    gap: var(--vs-space-8);
  }
}
</style>
