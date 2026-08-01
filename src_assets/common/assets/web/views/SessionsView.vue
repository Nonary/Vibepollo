<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
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
  type StatusTone,
} from '@/components/ui';
import type {
  RTSPSession,
  SessionStatus,
  SessionSummary,
  WebRTCSession,
} from '@/types/sessions';
import { formatBitrate, formatBytes, formatDuration, formatRelativeTime } from '@/utils/format';

const { locale, t } = useI18n();

interface SessionsPayload<T> {
  sessions?: T[];
}

interface HistoryStatus {
  available: boolean;
  degraded: boolean;
  dropped_samples: number;
  failed_writes: number;
  pending_control_commands: number;
  pending_priority_commands: number;
  pending_regular_commands: number;
  pending_samples: number;
}

interface HistoryPayload extends SessionsPayload<SessionSummary> {
  history_status?: HistoryStatus;
}

interface MutationResponse {
  status?: boolean | string;
  error?: string;
}

interface DiagnosticValue {
  label: string;
  value: string;
  monospace?: boolean;
}

interface ActiveSessionRow {
  key: string;
  id: string;
  protocol: 'RTSP' | 'WebRTC';
  client: string;
  app: string;
  state: string;
  tone: StatusTone;
  resolution: string;
  fps: string;
  bitrate: string;
  codec: string;
  duration: string;
  hdr: boolean;
  diagnostics: DiagnosticValue[];
}

type PendingAction =
  | { kind: 'stop-rtsp' | 'stop-webrtc'; session: ActiveSessionRow }
  | { kind: 'delete-history'; history: SessionSummary };

const rtspSessions = ref<RTSPSession[]>([]);
const webRtcSessions = ref<WebRTCSession[]>([]);
const activeRows = ref<ActiveSessionRow[]>([]);
const historyRows = ref<SessionSummary[]>([]);
const sessionStatus = ref<SessionStatus | null>(null);
const historyStatus = ref<HistoryStatus | null>(null);
const ready = ref(false);
const refreshing = ref(false);
const error = ref('');
const notice = ref('');
const lastUpdated = ref<number | null>(null);
const pendingAction = ref<PendingAction | null>(null);
const confirmOpen = ref(false);
const busyActionKey = ref('');
let refreshTimer: number | undefined;
let refreshInFlight = false;

const rtspCount = computed(() => activeRows.value.filter((row) => row.protocol === 'RTSP').length);
const webRtcCount = computed(
  () => activeRows.value.filter((row) => row.protocol === 'WebRTC').length,
);

const confirmTitle = computed(() => {
  const action = pendingAction.value;
  if (!action) return t('ui.sessions.confirm.generic_title');
  if (action.kind === 'delete-history') return t('ui.sessions.confirm.delete_title');
  return action.kind === 'stop-rtsp'
    ? t('ui.sessions.confirm.stop_rtsp_title')
    : t('ui.sessions.confirm.stop_webrtc_title');
});

const confirmDescription = computed(() => {
  const action = pendingAction.value;
  if (!action) return '';
  if (action.kind === 'delete-history') {
    return t('sessions.history_delete_confirm');
  }
  if (action.kind === 'stop-rtsp') {
    return rtspCount.value > 1
      ? t('ui.sessions.confirm.stop_rtsp_all_description')
      : t('ui.sessions.confirm.stop_rtsp_description');
  }
  return t('ui.sessions.confirm.stop_webrtc_description');
});

const confirmLabel = computed(() => {
  const kind = pendingAction.value?.kind;
  if (kind === 'delete-history') return t('ui.sessions.action.delete_record');
  return t('ui.sessions.action.stop_stream');
});

function stableReconcile<T>(current: T[], incoming: T[], keyFor: (item: T) => string): T[] {
  const incomingByKey = new Map(incoming.map((item) => [keyFor(item), item]));
  const stable = current.flatMap((item) => {
    const key = keyFor(item);
    const replacement = incomingByKey.get(key);
    if (!replacement) return [];
    incomingByKey.delete(key);
    return [replacement];
  });
  return [...stable, ...incomingByKey.values()];
}

function resolution(width?: number, height?: number): string {
  return width && height ? `${width} × ${height}` : t('ui.sessions.value.not_reported');
}

function numeric(value: number | undefined, formatKey?: string): string {
  if (!Number.isFinite(value)) return t('ui.sessions.value.not_reported');
  return formatKey
    ? t(formatKey, { value })
    : Number(value).toLocaleString(locale.value || undefined);
}

function localizedSessionState(value: string): string {
  const key = value.trim().toLocaleLowerCase().replace(/[_-]+/g, '_');
  const knownStates: Record<string, string> = {
    connected: 'clients.connected',
    error: 'ui.sessions.status.error',
    paused: 'ui.sessions.status.paused',
    stopped: 'ui.sessions.status.stopped',
    stopping: 'ui.sessions.status.stopping',
    streaming: 'ui.sessions.status.streaming',
  };
  return knownStates[key] ? t(knownStates[key]) : t('_common.unknown');
}

function rtspState(session: RTSPSession): { label: string; tone: StatusTone } {
  const state = session.state.toLocaleLowerCase();
  if (state.includes('stop') || state.includes('error')) {
    return { label: localizedSessionState(state), tone: 'warning' };
  }
  return { label: state ? localizedSessionState(state) : t('ui.sessions.status.streaming'), tone: 'success' };
}

function webRtcState(session: WebRTCSession): { label: string; tone: StatusTone } {
  if (!session.has_remote_offer) return { label: t('ui.sessions.status.awaiting_offer'), tone: 'warning' };
  if (!session.has_local_answer) return { label: t('ui.sessions.status.negotiating'), tone: 'info' };
  if (session.video_packets > 0 || session.audio_packets > 0) {
    return { label: t('ui.sessions.status.streaming'), tone: 'success' };
  }
  return { label: t('clients.connected'), tone: 'success' };
}

function normalizeRtsp(session: RTSPSession): ActiveSessionRow {
  const state = rtspState(session);
  return {
    key: `rtsp:${session.uuid}`,
    id: session.uuid,
    protocol: 'RTSP',
    client: session.device_name || t('ui.sessions.value.moonlight_client'),
    app: sessionStatus.value?.appName || t('ui.sessions.value.desktop_stream'),
    state: state.label,
    tone: state.tone,
    resolution: resolution(session.width, session.height),
    fps: numeric(session.fps, 'ui.sessions.value.fps'),
    bitrate: formatBitrate(session.encoder_bitrate_kbps, locale.value),
    codec: session.codec || t('ui.sessions.value.unknown_codec'),
    duration: formatDuration(session.uptime_seconds, locale.value),
    hdr: session.hdr,
    diagnostics: [
      { label: t('ui.sessions.diagnostic.session_id'), value: session.uuid, monospace: true },
      {
        label: t('sessions.stream_gpu_model'),
        value: session.stream_gpu_model || t('ui.sessions.value.not_reported'),
      },
      { label: t('sessions.audio_channels'), value: numeric(session.audio_channels) },
      { label: t('sessions.data_sent'), value: formatBytes(session.bytes_sent, locale.value) },
      {
        label: t('sessions.frames_sent'),
        value: session.frames_sent.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.packets_sent'),
        value: session.packets_sent.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.client_losses'),
        value: session.client_reported_losses.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.idr_requests'),
        value: session.idr_requests.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.frame_invalidations'),
        value: session.invalidate_ref_count.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.encode_latency'),
        value: numeric(session.encode_latency_ms, 'ui.sessions.value.milliseconds'),
      },
    ],
  };
}

function normalizeWebRtc(session: WebRTCSession): ActiveSessionRow {
  const state = webRtcState(session);
  return {
    key: `webrtc:${session.id}`,
    id: session.id,
    protocol: 'WebRTC',
    client: t('ui.sessions.value.browser_session', { id: session.id.slice(0, 8) }),
    app: t('ui.sessions.value.browser_stream'),
    state: state.label,
    tone: state.tone,
    resolution: resolution(session.width, session.height),
    fps: numeric(session.fps, 'ui.sessions.value.fps'),
    bitrate: formatBitrate(session.encoder_bitrate_kbps ?? 0, locale.value),
    codec: session.codec || t('ui.sessions.value.negotiating_codec'),
    duration: t('sessions.live'),
    hdr: session.hdr ?? false,
    diagnostics: [
      { label: t('ui.sessions.diagnostic.session_id'), value: session.id, monospace: true },
      {
        label: t('sessions.stream_gpu_model'),
        value: session.stream_gpu_model || t('ui.sessions.value.not_reported'),
      },
      {
        label: t('sessions.audio_codec'),
        value: session.audio_codec || t('ui.sessions.value.not_reported'),
      },
      { label: t('sessions.audio_channels'), value: numeric(session.audio_channels) },
      { label: t('sessions.data_sent'), value: formatBytes(session.bytes_sent, locale.value) },
      {
        label: t('sessions.video_packets'),
        value: session.video_packets.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.audio_packets'),
        value: session.audio_packets.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.video_dropped'),
        value: session.video_dropped.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.audio_dropped'),
        value: session.audio_dropped.toLocaleString(locale.value || undefined),
      },
      {
        label: t('ui.sessions.diagnostic.ice_candidates'),
        value: session.ice_candidates.toLocaleString(locale.value || undefined),
      },
      {
        label: t('sessions.video_queue'),
        value: t(
          'ui.sessions.value.frame_count',
          { count: session.video_queue_frames },
          session.video_queue_frames,
        ),
      },
      {
        label: t('sessions.last_video'),
        value: numeric(session.last_video_age_ms, 'ui.sessions.value.milliseconds_ago'),
      },
    ],
  };
}

function rebuildActiveRows(): void {
  const incoming = [
    ...rtspSessions.value.map(normalizeRtsp),
    ...webRtcSessions.value.map(normalizeWebRtc),
  ];
  activeRows.value = stableReconcile(activeRows.value, incoming, (row) => row.key);
}

function settledMessage(result: PromiseSettledResult<unknown>, labelKey: string): string | null {
  if (result.status === 'fulfilled') return null;
  const source = t(labelKey);
  const detail =
    result.reason instanceof ApiError
      ? t('ui.sessions.error.source_load', { source })
      : result.reason instanceof Error
      ? result.reason.message
      : t('ui.sessions.error.source_load', { source });
  return t('ui.sessions.error.source_detail', { source, detail });
}

async function refreshSessions(silent = false): Promise<void> {
  if (refreshInFlight) return;
  refreshInFlight = true;
  if (!silent) refreshing.value = true;

  const results = await Promise.allSettled([
    apiGet<SessionStatus>('/api/session/status'),
    apiGet<SessionsPayload<RTSPSession>>('/api/rtsp/sessions'),
    apiGet<SessionsPayload<WebRTCSession>>('/api/webrtc/sessions'),
    apiGet<HistoryPayload>('/api/history/sessions?limit=50&offset=0'),
  ]);

  const [statusResult, rtspResult, webRtcResult, historyResult] = results;
  if (statusResult.status === 'fulfilled') sessionStatus.value = statusResult.value;
  if (rtspResult.status === 'fulfilled') {
    rtspSessions.value = Array.isArray(rtspResult.value.sessions) ? rtspResult.value.sessions : [];
  }
  if (webRtcResult.status === 'fulfilled') {
    webRtcSessions.value = Array.isArray(webRtcResult.value.sessions)
      ? webRtcResult.value.sessions
      : [];
  }
  if (historyResult.status === 'fulfilled') {
    const incoming = Array.isArray(historyResult.value.sessions) ? historyResult.value.sessions : [];
    historyRows.value = stableReconcile(historyRows.value, incoming, (row) => row.uuid);
    historyStatus.value = historyResult.value.history_status ?? null;
  }
  rebuildActiveRows();

  const messages = [
    settledMessage(statusResult, 'ui.sessions.source.host_status'),
    settledMessage(rtspResult, 'ui.sessions.source.rtsp'),
    settledMessage(webRtcResult, 'ui.sessions.source.webrtc'),
    settledMessage(historyResult, 'sessions.history_title'),
  ].filter((message): message is string => Boolean(message));
  error.value = messages.join(' ');
  if (results.some((result) => result.status === 'fulfilled')) lastUpdated.value = Date.now();

  ready.value = true;
  refreshing.value = false;
  refreshInFlight = false;
}

function requestStop(session: ActiveSessionRow): void {
  pendingAction.value = {
    kind: session.protocol === 'RTSP' ? 'stop-rtsp' : 'stop-webrtc',
    session,
  };
  confirmOpen.value = true;
}

function requestHistoryDelete(history: SessionSummary): void {
  pendingAction.value = { kind: 'delete-history', history };
  confirmOpen.value = true;
}

function clearPendingAction(): void {
  if (!busyActionKey.value) pendingAction.value = null;
}

function actionKey(action: PendingAction): string {
  return action.kind === 'delete-history' ? `history:${action.history.uuid}` : action.session.key;
}

async function confirmAction(): Promise<void> {
  const action = pendingAction.value;
  if (!action) return;
  error.value = '';
  notice.value = '';
  busyActionKey.value = actionKey(action);

  try {
    if (action.kind === 'stop-rtsp') {
      const response = await apiPost<MutationResponse>('/api/apps/close', {});
      if (response.status !== true) throw new Error(t('ui.sessions.error.stop_rtsp_rejected'));
      notice.value = t('ui.sessions.notice.stop_rtsp');
    } else if (action.kind === 'stop-webrtc') {
      const response = await apiDelete<MutationResponse>(
        `/api/webrtc/sessions/${encodeURIComponent(action.session.id)}`,
      );
      if (response.status !== true) {
        throw new Error(response.error || t('ui.sessions.error.webrtc_not_found'));
      }
      notice.value = t('ui.sessions.notice.stop_webrtc');
    } else if (action.kind === 'delete-history') {
      await apiDelete<MutationResponse>(
        `/api/history/sessions/${encodeURIComponent(action.history.uuid)}`,
      );
      historyRows.value = historyRows.value.filter((row) => row.uuid !== action.history.uuid);
      notice.value = t('ui.sessions.notice.history_deleted');
    }

    confirmOpen.value = false;
    pendingAction.value = null;
    await refreshSessions(true);
  } catch (cause) {
    error.value =
      cause instanceof ApiError
        ? t('ui.sessions.error.action')
        : cause instanceof Error
          ? cause.message
          : t('ui.sessions.error.action');
  } finally {
    busyActionKey.value = '';
  }
}

function historyTone(verdict?: string): StatusTone {
  if (verdict === 'healthy') return 'success';
  if (verdict === 'degraded') return 'warning';
  if (verdict === 'failed') return 'danger';
  return 'neutral';
}

function historyVerdict(verdict?: string): string {
  if (verdict === 'healthy') return t('sessions.history_verdict_healthy');
  if (verdict === 'degraded') return t('sessions.history_verdict_degraded');
  if (verdict === 'failed') return t('sessions.history_verdict_failed');
  return t('sessions.history_verdict_unknown');
}

function unixDate(timestamp: number | undefined): Date | null {
  if (!timestamp || !Number.isFinite(timestamp)) return null;
  return new Date(timestamp < 1_000_000_000_000 ? timestamp * 1000 : timestamp);
}

function historyWhen(history: SessionSummary): string {
  const date = unixDate(history.end_time_unix || history.start_time_unix);
  return date
    ? formatRelativeTime(date, locale.value, t('ui.sessions.value.unknown_time'))
    : t('ui.sessions.value.unknown_time');
}

function protocolLabel(protocol: ActiveSessionRow['protocol']): string {
  return protocol === 'RTSP' ? t('ui.sessions.protocol.rtsp') : t('ui.sessions.protocol.webrtc');
}

function historyDateTime(history: SessionSummary): string {
  const date = unixDate(history.end_time_unix || history.start_time_unix);
  if (!date) return '';
  return new Intl.DateTimeFormat(locale.value || undefined, {
    dateStyle: 'medium',
    timeStyle: 'short',
  }).format(date);
}

onMounted(() => {
  void refreshSessions();
  refreshTimer = window.setInterval(() => {
    if (!document.hidden) void refreshSessions(true);
  }, 4000);
});

onBeforeUnmount(() => {
  if (refreshTimer !== undefined) window.clearInterval(refreshTimer);
});
</script>

<template>
  <div class="vs-page vs-page--dashboard sessions-page">
    <PageHeader
      :title="t('ui.sessions.page.title')"
      :description="t('ui.sessions.page.description')"
    >
      <template #meta>
        <StatusBadge
          :label="
            activeRows.length
              ? t('ui.sessions.count.active', { count: activeRows.length }, activeRows.length)
              : t('ui.sessions.status.no_active')
          "
          :tone="activeRows.length ? 'success' : 'neutral'"
          compact
        />
        <StatusBadge
          v-if="rtspCount"
          :label="t('ui.sessions.count.rtsp', { count: rtspCount }, rtspCount)"
          tone="info"
          compact
        />
        <StatusBadge
          v-if="webRtcCount"
          :label="t('ui.sessions.count.webrtc', { count: webRtcCount }, webRtcCount)"
          tone="info"
          compact
        />
        <span v-if="lastUpdated" class="last-updated">
          {{
            t('clients.last_updated', {
              time: formatRelativeTime(lastUpdated, locale, t('ui.sessions.value.unknown_time')),
            })
          }}
        </span>
      </template>
      <template #actions>
        <RouterLink class="vs-button vs-button--secondary vs-button--default" to="/stats">
          {{ t('stats.title') }}
        </RouterLink>
        <AppButton
          :label="t('_common.refresh')"
          icon="refresh"
          :busy="refreshing"
          :busy-label="t('ui.sessions.action.refreshing')"
          @click="refreshSessions()"
        />
      </template>
    </PageHeader>

    <div class="session-stack">
      <InlineAlert
        v-if="error"
        tone="danger"
        :title="t('ui.sessions.alert.partial_failure_title')"
        announce="polite"
      >
        {{ error }} {{ t('ui.sessions.alert.partial_failure_description') }}
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
      <InlineAlert
        v-if="sessionStatus?.paused"
        tone="warning"
        :title="t('ui.sessions.alert.app_running_title')"
      >
        {{
          t('ui.sessions.alert.app_running_description', {
            app: sessionStatus.appName || t('ui.sessions.value.streamed_application'),
          })
        }}
      </InlineAlert>
      <InlineAlert
        v-if="historyStatus && (!historyStatus.available || historyStatus.degraded)"
        tone="warning"
        :title="t('ui.sessions.alert.history_degraded_title')"
      >
        {{ t('ui.sessions.alert.history_degraded_description') }}
      </InlineAlert>

      <section class="session-section" aria-labelledby="active-sessions-title">
        <div class="section-heading">
          <div>
            <h2 id="active-sessions-title">{{ t('ui.sessions.active.title') }}</h2>
            <p>{{ t('ui.sessions.active.refresh_description') }}</p>
          </div>
          <span class="section-count">{{ activeRows.length }}</span>
        </div>

        <div v-if="!ready" class="active-loading">
          <LoadingSkeleton v-for="item in 2" :key="item" variant="block" height="13rem" />
        </div>

        <EmptyState
          v-else-if="!activeRows.length"
          :title="t('ui.sessions.status.no_active')"
          :description="t('ui.sessions.active.empty_description')"
          icon="sessions"
          compact
        />

        <ul v-else class="active-list" :aria-label="t('ui.sessions.active.list_aria_label')">
          <li v-for="session in activeRows" :key="session.key">
            <article class="session-card vs-surface" :aria-labelledby="`session-title-${session.key}`">
              <div class="session-card__header">
                <div class="session-card__identity">
                  <div class="session-card__badges">
                    <StatusBadge :label="session.state" :tone="session.tone" compact />
                    <StatusBadge :label="protocolLabel(session.protocol)" tone="neutral" compact />
                    <StatusBadge
                      v-if="session.hdr"
                      :label="t('sessions.history_hdr')"
                      tone="info"
                      compact
                    />
                  </div>
                  <h3 :id="`session-title-${session.key}`">{{ session.client }}</h3>
                  <p>{{ session.app }}</p>
                </div>
                <AppButton
                  :label="t('ui.sessions.action.stop_stream')"
                  icon="stop"
                  variant="secondary"
                  :disabled="Boolean(busyActionKey)"
                  :aria-label="
                    t('ui.sessions.action.stop_stream_named', {
                      protocol: protocolLabel(session.protocol),
                      client: session.client,
                    })
                  "
                  @click="requestStop(session)"
                />
              </div>

              <dl class="session-metrics">
                <div>
                  <dt>{{ t('ui.sessions.metric.picture') }}</dt>
                  <dd>{{ session.resolution }} · {{ session.fps }}</dd>
                </div>
                <div>
                  <dt>{{ t('sessions.video') }}</dt>
                  <dd>{{ session.codec }} · {{ session.bitrate }}</dd>
                </div>
                <div>
                  <dt>{{ t('sessions.history_duration') }}</dt>
                  <dd>{{ session.duration }}</dd>
                </div>
              </dl>

              <details class="session-diagnostics">
                <summary>{{ t('ui.sessions.diagnostic.title') }}</summary>
                <dl>
                  <div v-for="diagnostic in session.diagnostics" :key="diagnostic.label">
                    <dt>{{ diagnostic.label }}</dt>
                    <dd :class="{ 'diagnostic-id': diagnostic.monospace }">
                      {{ diagnostic.value }}
                    </dd>
                  </div>
                </dl>
              </details>
            </article>
          </li>
        </ul>
      </section>

      <section class="session-section" aria-labelledby="session-history-title">
        <div class="section-heading">
          <div>
            <h2 id="session-history-title">{{ t('sessions.history_title') }}</h2>
            <p>{{ t('ui.sessions.history.description') }}</p>
          </div>
          <span class="section-count">{{ historyRows.length }}</span>
        </div>

        <EmptyState
          v-if="ready && !historyRows.length"
          :title="t('ui.sessions.history.empty_title')"
          :description="t('ui.sessions.history.empty_description')"
          icon="logs"
          compact
        />

        <div v-else-if="historyRows.length" class="vs-table-wrap">
          <table class="vs-table vs-table--responsive history-table">
            <caption class="vs-sr-only">
              {{ t('ui.sessions.history.table_caption') }}
            </caption>
            <thead>
              <tr>
                <th scope="col">{{ t('ui.sessions.history.client_and_game') }}</th>
                <th scope="col">{{ t('sessions.history_ended') }}</th>
                <th scope="col">{{ t('ui.sessions.history.stream') }}</th>
                <th scope="col">{{ t('sessions.history_duration') }}</th>
                <th scope="col">{{ t('ui.sessions.history.outcome') }}</th>
                <th scope="col">
                  <span class="vs-sr-only">{{ t('ui.sessions.history.actions') }}</span>
                </th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="history in historyRows" :key="history.uuid">
                <td :data-label="t('ui.sessions.history.client_and_game')">
                  <strong>
                    {{
                      history.client_name ||
                      history.device_name ||
                      t('ui.sessions.value.unknown_client')
                    }}
                  </strong>
                  <span>{{ history.app_name || t('ui.sessions.value.desktop_stream') }}</span>
                  <details class="history-details">
                    <summary>{{ t('sessions.history_view_detail') }}</summary>
                    <dl>
                      <div>
                        <dt>{{ t('sessions.history_protocol') }}</dt>
                        <dd>{{ history.protocol || t('_common.unknown') }}</dd>
                      </div>
                      <div>
                        <dt>{{ t('sessions.codec') }}</dt>
                        <dd>{{ history.codec || t('_common.unknown') }}</dd>
                      </div>
                      <div>
                        <dt>{{ t('sessions.bitrate') }}</dt>
                        <dd>{{ formatBitrate(history.encoder_bitrate_kbps, locale) }}</dd>
                      </div>
                      <div>
                        <dt>{{ t('ui.sessions.diagnostic.session_id') }}</dt>
                        <dd class="diagnostic-id">{{ history.uuid }}</dd>
                      </div>
                    </dl>
                  </details>
                </td>
                <td :data-label="t('sessions.history_ended')">
                  <time :datetime="unixDate(history.end_time_unix || history.start_time_unix)?.toISOString()">
                    {{ historyWhen(history) }}
                  </time>
                  <span>{{ historyDateTime(history) }}</span>
                </td>
                <td :data-label="t('ui.sessions.history.stream')">
                  <strong>{{ resolution(history.width, history.height) }}</strong>
                  <span>
                    {{ t('ui.sessions.value.fps', { value: history.target_fps }) }} ·
                    {{ history.codec || t('_common.unknown') }}
                  </span>
                </td>
                <td :data-label="t('sessions.history_duration')">
                  {{ formatDuration(history.duration_seconds, locale) }}
                </td>
                <td :data-label="t('ui.sessions.history.outcome')">
                  <StatusBadge
                    :label="historyVerdict(history.verdict)"
                    :tone="historyTone(history.verdict)"
                    compact
                  />
                </td>
                <td class="vs-table__actions" :data-label="t('ui.sessions.history.actions')">
                  <AppButton
                    :label="t('ui.sessions.action.delete_record')"
                    :aria-label="t('ui.sessions.action.delete_record')"
                    icon="trash"
                    icon-only
                    variant="tertiary"
                    size="compact"
                    :disabled="Boolean(busyActionKey)"
                    @click="requestHistoryDelete(history)"
                  />
                </td>
              </tr>
            </tbody>
          </table>
        </div>
      </section>
    </div>

    <ConfirmDialog
      v-model:open="confirmOpen"
      :title="confirmTitle"
      :description="confirmDescription"
      :confirm-label="confirmLabel"
      :cancel-label="t('_common.cancel')"
      :tone="pendingAction?.kind === 'delete-history' ? 'danger' : 'default'"
      :busy="Boolean(pendingAction && busyActionKey === actionKey(pendingAction))"
      :busy-label="t('ui.sessions.action.working')"
      :close-on-confirm="false"
      @confirm="confirmAction"
      @cancel="clearPendingAction"
    />
  </div>
</template>

<style scoped>
.sessions-page,
.session-stack,
.session-section,
.active-list,
.active-loading {
  display: grid;
}

.sessions-page,
.session-stack {
  gap: var(--vs-space-24);
}

.session-section,
.active-list,
.active-loading {
  gap: var(--vs-space-12);
}

.last-updated,
.section-heading p,
.history-table td > span {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-metadata);
}

.section-heading {
  display: flex;
  align-items: end;
  justify-content: space-between;
  gap: var(--vs-space-16);
}

.section-heading h2 {
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
}

.section-heading p {
  margin-top: var(--vs-space-4);
}

.section-count {
  min-width: 2rem;
  color: var(--vs-color-text-secondary);
  font-variant-numeric: tabular-nums;
  text-align: end;
}

.active-list {
  padding: 0;
  list-style: none;
}

.session-card {
  overflow: clip;
}

.session-card__header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: var(--vs-space-20);
  padding: var(--vs-space-20);
}

.session-card__identity {
  min-width: 0;
}

.session-card__badges {
  display: flex;
  flex-wrap: wrap;
  gap: var(--vs-space-8);
  margin-bottom: var(--vs-space-8);
}

.session-card h3 {
  overflow: hidden;
  font-size: var(--vs-type-size-section);
  line-height: var(--vs-type-line-height-section);
  text-overflow: ellipsis;
  white-space: nowrap;
}

.session-card__identity > p {
  margin-top: var(--vs-space-2);
  color: var(--vs-color-text-secondary);
}

.session-metrics {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  border-top: 1px solid var(--vs-color-border-subtle);
  border-bottom: 1px solid var(--vs-color-border-subtle);
}

.session-metrics > div {
  min-width: 0;
  padding: var(--vs-space-16) var(--vs-space-20);
}

.session-metrics > div + div {
  border-left: 1px solid var(--vs-color-border-subtle);
}

.session-metrics dt,
.session-diagnostics dt,
.history-details dt {
  color: var(--vs-color-text-muted);
  font-size: var(--vs-type-size-helper);
  font-weight: var(--vs-type-weight-medium);
  text-transform: uppercase;
  letter-spacing: 0.03em;
}

.session-metrics dd {
  margin-top: var(--vs-space-4);
  overflow: hidden;
  color: var(--vs-color-text-primary);
  font-size: var(--vs-type-size-control);
  font-variant-numeric: tabular-nums;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.session-diagnostics summary,
.history-details summary {
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-control);
  font-weight: var(--vs-type-weight-medium);
  cursor: pointer;
}

.session-diagnostics > summary {
  min-height: 44px;
  padding: var(--vs-space-12) var(--vs-space-20);
}

.session-diagnostics > dl {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: var(--vs-space-16);
  padding: 0 var(--vs-space-20) var(--vs-space-20);
}

.session-diagnostics dd,
.history-details dd {
  margin-top: var(--vs-space-2);
  overflow-wrap: anywhere;
  color: var(--vs-color-text-secondary);
  font-size: var(--vs-type-size-metadata);
  font-variant-numeric: tabular-nums;
}

.diagnostic-id {
  font-family: var(--vs-type-family-mono);
}

.history-table td:first-child,
.history-table td:nth-child(2),
.history-table td:nth-child(3) {
  min-width: 11rem;
}

.history-table td > strong,
.history-table td > span {
  display: block;
}

.history-table time {
  display: block;
  font-variant-numeric: tabular-nums;
}

.history-details {
  margin-top: var(--vs-space-8);
}

.history-details summary {
  min-height: 32px;
  width: fit-content;
  padding: var(--vs-space-4) 0;
}

.history-details dl {
  display: grid;
  gap: var(--vs-space-8);
  margin-top: var(--vs-space-8);
}

@media (max-width: 767px) {
  .session-card__header {
    align-items: stretch;
    flex-direction: column;
  }

  .session-metrics {
    grid-template-columns: minmax(0, 1fr);
  }

  .session-metrics > div + div {
    border-top: 1px solid var(--vs-color-border-subtle);
    border-left: 0;
  }

  .session-diagnostics > dl {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .history-table td {
    min-width: 0 !important;
  }

  .history-table .vs-table__actions :deep(.vs-button) {
    width: 44px;
    height: 44px;
  }
}

@media (max-width: 479px) {
  .session-diagnostics > dl {
    grid-template-columns: minmax(0, 1fr);
  }
}
</style>
