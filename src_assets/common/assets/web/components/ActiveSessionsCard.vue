<template>
  <n-card class="mb-8" :segmented="{ content: true, footer: false }">
    <template #header>
      <div class="flex flex-wrap items-center justify-between gap-3">
        <div class="flex items-center gap-3">
          <h2 class="text-lg font-medium flex items-center gap-2">
            <i class="fas fa-broadcast-tower" /> {{ t('sessions.title') }}
          </h2>
          <span
            :class="[
              'h-2.5 w-2.5 rounded-full',
              hasActiveSessions ? 'bg-success animate-pulse ring-2 ring-success/30' : 'bg-dark/30 dark:bg-light/30',
            ]"
          />
          <span class="text-xs font-medium opacity-75">
            {{ hasActiveSessions ? t('sessions.live') : t('sessions.idle') }}
          </span>
        </div>
        <n-button size="small" :loading="loading" @click="refresh">
          <i class="fas fa-rotate" />
          <span class="ml-2">{{ t('sessions.refresh') }}</span>
        </n-button>
      </div>
    </template>

    <div v-if="!hasActiveSessions && !loading" class="text-sm opacity-60 py-2">
      {{ t('sessions.no_active') }}
    </div>

    <!-- RTSP Sessions -->
    <div v-if="rtspSessions.length > 0" class="mb-4">
      <div class="flex items-center gap-2 mb-3">
        <n-tag type="info" size="small" :bordered="false">RTSP</n-tag>
        <span class="text-sm font-medium">
          {{ t('sessions.rtsp_active', { count: rtspSessions.length }) }}
        </span>
        <n-tag v-if="appRunning" type="success" size="small" :bordered="false">
          <span class="inline-flex items-center"><i class="fas fa-gamepad mr-1" />{{ appName || t('sessions.app_running') }}</span>
        </n-tag>
      </div>

      <div class="space-y-3">
        <div
          v-for="session in rtspSessions"
          :key="session.uuid"
          class="rounded-xl border border-dark/[0.06] bg-light/[0.03] p-4 dark:border-light/[0.10] dark:bg-dark/[0.06]"
        >
          <div class="flex flex-wrap items-center gap-2 mb-3">
            <span class="text-sm font-semibold">{{ session.device_name || session.uuid.substring(0, 8) }}</span>
            <n-tag type="success" size="small" :bordered="false">
              <span class="inline-flex items-center"><i class="fas fa-video mr-1" />{{ t('sessions.video') }}</span>
            </n-tag>
            <n-tag v-if="session.hdr"type="warning" size="small" :bordered="false">HDR</n-tag>
            <n-tag type="default" size="small" :bordered="false">{{ session.state }}</n-tag>
          </div>

          <div class="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-6 gap-3">
            <div v-if="session.width && session.height" class="stat-cell">
              <div class="stat-label">{{ t('sessions.resolution') }}</div>
              <div class="stat-value">{{ session.width }}×{{ session.height }}</div>
            </div>
            <div v-if="session.fps" class="stat-cell">
              <div class="stat-label">{{ t('sessions.fps') }}</div>
              <div class="stat-value">{{ session.fps }}</div>
            </div>
            <div v-if="session.bitrate_kbps" class="stat-cell">
              <div class="stat-label">{{ t('sessions.bitrate') }}</div>
              <div class="stat-value">{{ formatBitrate(session.bitrate_kbps) }}</div>
            </div>
            <div v-if="session.codec" class="stat-cell">
              <div class="stat-label">{{ t('sessions.codec') }}</div>
              <div class="stat-value">{{ session.codec }}</div>
            </div>
            <div v-if="session.audio_channels" class="stat-cell">
              <div class="stat-label">{{ t('sessions.audio_channels') }}</div>
              <div class="stat-value">{{ session.audio_channels }}ch</div>
            </div>

            <!-- Real-time performance stats -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.encode_latency') }}</div>
              <div :class="['stat-value', session.encode_latency_ms > 16 ? 'text-danger' : session.encode_latency_ms > 8 ? 'text-warning' : '']">
                {{ session.encode_latency_ms.toFixed(1) }}ms
              </div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.frames_sent') }}</div>
              <div class="stat-value">{{ formatNumber(session.frames_sent) }}</div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.packets_sent') }}</div>
              <div class="stat-value">{{ formatNumber(session.packets_sent) }}</div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.data_sent') }}</div>
              <div class="stat-value">{{ formatBytes(session.bytes_sent) }}</div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.client_losses') }}</div>
              <div :class="['stat-value', session.client_reported_losses > 0 ? 'text-danger' : '']">
                {{ formatNumber(session.client_reported_losses) }}
              </div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.idr_requests') }}</div>
              <div :class="['stat-value', session.idr_requests > 10 ? 'text-warning' : '']">
                {{ session.idr_requests }}
              </div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.frame_invalidations') }}</div>
              <div :class="['stat-value', session.invalidate_ref_count > 0 ? 'text-warning' : '']">
                {{ session.invalidate_ref_count }}
              </div>
            </div>
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.uptime') }}</div>
              <div class="stat-value">{{ formatUptime(session.uptime_seconds) }}</div>
            </div>
          </div>
        </div>
      </div>

      <!-- Charts toggle -->
      <div class="flex items-center gap-2 mt-3">
        <n-button size="small" :type="showCharts ? 'primary' : 'default'" @click="showCharts = !showCharts">
          <i :class="['fas', showCharts ? 'fa-chart-line' : 'fa-chart-line']" />
          <span class="ml-2">{{ showCharts ? t('sessions.hide_charts') : t('sessions.show_charts') }}</span>
        </n-button>
      </div>

      <!-- Session Charts -->
      <SessionCharts v-if="showCharts && rtspSessions.length > 0" :session="rtspSessions[0]" />
    </div>
    <div v-else-if="rtspCount > 0" class="mb-4">
      <div class="flex items-center gap-2 mb-3">
        <n-tag type="info" size="small" :bordered="false">RTSP</n-tag>
        <span class="text-sm font-medium">
          {{ t('sessions.rtsp_active', { count: rtspCount }) }}
        </span>
        <n-tag v-if="appRunning" type="success" size="small" :bordered="false">
          <span class="inline-flex items-center"><i class="fas fa-gamepad mr-1" />{{ appName || t('sessions.app_running') }}</span>
        </n-tag>
      </div>
    </div>

    <!-- WebRTC Sessions -->
    <div v-if="webrtcSessions.length > 0">
      <div class="flex items-center gap-2 mb-3">
        <n-tag type="warning" size="small" :bordered="false">WebRTC</n-tag>
        <span class="text-sm font-medium">
          {{ t('sessions.webrtc_active', { count: webrtcSessions.length }) }}
        </span>
      </div>

      <div class="space-y-3">
        <div
          v-for="session in webrtcSessions"
          :key="session.id"
          class="rounded-xl border border-dark/[0.06] bg-light/[0.03] p-4 dark:border-light/[0.10] dark:bg-dark/[0.06]"
        >
          <!-- Session header -->
          <div class="flex flex-wrap items-center gap-2 mb-3">
            <span class="text-sm font-semibold">{{ session.id.substring(0, 8) }}</span>
            <n-tag v-if="session.video" type="success" size="small" :bordered="false">
              <span class="inline-flex items-center"><i class="fas fa-video mr-1" />{{ t('sessions.video') }}</span>
            </n-tag>
            <n-tag v-if="session.audio" type="success" size="small" :bordered="false">
              <span class="inline-flex items-center"><i class="fas fa-volume-up mr-1" />{{ t('sessions.audio') }}</span>
            </n-tag>
            <n-tag v-if="session.encoded" type="info" size="small" :bordered="false">
              {{ t('sessions.encoded') }}
            </n-tag>
            <n-tag v-if="session.hdr" type="warning" size="small" :bordered="false">HDR</n-tag>
          </div>

          <!-- Stats grid -->
          <div class="grid grid-cols-2 sm:grid-cols-3 lg:grid-cols-4 xl:grid-cols-6 gap-3">
            <!-- Resolution -->
            <div v-if="session.width && session.height" class="stat-cell">
              <div class="stat-label">{{ t('sessions.resolution') }}</div>
              <div class="stat-value">{{ session.width }}×{{ session.height }}</div>
            </div>

            <!-- FPS -->
            <div v-if="session.fps != null" class="stat-cell">
              <div class="stat-label">{{ t('sessions.fps') }}</div>
              <div class="stat-value">{{ session.fps }}</div>
            </div>

            <!-- Bitrate -->
            <div v-if="session.bitrate_kbps != null" class="stat-cell">
              <div class="stat-label">{{ t('sessions.bitrate') }}</div>
              <div class="stat-value">{{ formatBitrate(session.bitrate_kbps) }}</div>
            </div>

            <!-- Codec -->
            <div v-if="session.codec" class="stat-cell">
              <div class="stat-label">{{ t('sessions.codec') }}</div>
              <div class="stat-value">{{ session.codec }}</div>
            </div>

            <!-- Video Packets -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.video_packets') }}</div>
              <div class="stat-value">{{ formatNumber(session.video_packets) }}</div>
            </div>

            <!-- Audio Packets -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.audio_packets') }}</div>
              <div class="stat-value">{{ formatNumber(session.audio_packets) }}</div>
            </div>

            <!-- Video Dropped -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.video_dropped') }}</div>
              <div :class="['stat-value', session.video_dropped > 0 ? 'text-danger' : '']">
                {{ formatNumber(session.video_dropped) }}
              </div>
            </div>

            <!-- Audio Dropped -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.audio_dropped') }}</div>
              <div :class="['stat-value', session.audio_dropped > 0 ? 'text-danger' : '']">
                {{ formatNumber(session.audio_dropped) }}
              </div>
            </div>

            <!-- Video Queue -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.video_queue') }}</div>
              <div class="stat-value">{{ session.video_queue_frames }}</div>
            </div>

            <!-- Audio Queue -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.audio_queue') }}</div>
              <div class="stat-value">{{ session.audio_queue_frames }}</div>
            </div>

            <!-- In-flight Frames -->
            <div class="stat-cell">
              <div class="stat-label">{{ t('sessions.inflight') }}</div>
              <div class="stat-value">{{ session.video_inflight_frames }}</div>
            </div>

            <!-- Audio Codec -->
            <div v-if="session.audio_codec" class="stat-cell">
              <div class="stat-label">{{ t('sessions.audio_codec') }}</div>
              <div class="stat-value">{{ session.audio_codec }}</div>
            </div>

            <!-- Profile -->
            <div v-if="session.profile" class="stat-cell">
              <div class="stat-label">{{ t('sessions.profile') }}</div>
              <div class="stat-value">{{ session.profile }}</div>
            </div>

            <!-- Last Video Age -->
            <div v-if="session.last_video_age_ms != null" class="stat-cell">
              <div class="stat-label">{{ t('sessions.last_video') }}</div>
              <div class="stat-value">{{ session.last_video_age_ms }}ms</div>
            </div>

            <!-- Last Audio Age -->
            <div v-if="session.last_audio_age_ms != null" class="stat-cell">
              <div class="stat-label">{{ t('sessions.last_audio') }}</div>
              <div class="stat-value">{{ session.last_audio_age_ms }}ms</div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </n-card>
</template>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useI18n } from 'vue-i18n';
import { http } from '@/http';
import { NButton, NCard, NTag } from 'naive-ui';
import { useAuthStore } from '@/stores/auth';
import SessionCharts from './SessionCharts.vue';

const { t } = useI18n();
const auth = useAuthStore();

interface SessionStatus {
  activeSessions: number;
  appRunning: boolean;
  appName: string;
  paused: boolean;
  status: boolean;
}

interface WebRTCSession {
  id: string;
  audio: boolean;
  video: boolean;
  encoded: boolean;
  audio_packets: number;
  video_packets: number;
  audio_dropped: number;
  video_dropped: number;
  audio_queue_frames: number;
  video_queue_frames: number;
  video_inflight_frames: number;
  has_remote_offer: boolean;
  has_local_answer: boolean;
  ice_candidates: number;
  width: number | null;
  height: number | null;
  fps: number | null;
  bitrate_kbps: number | null;
  codec: string | null;
  hdr: boolean | null;
  audio_channels: number | null;
  audio_codec: string | null;
  profile: string | null;
  video_pacing_mode: string | null;
  video_pacing_slack_ms: number | null;
  video_max_frame_age_ms: number | null;
  last_audio_bytes: number;
  last_video_bytes: number;
  last_video_idr: boolean;
  last_video_frame_index: number;
  last_audio_age_ms: number | null;
  last_video_age_ms: number | null;
}

interface RTSPSession {
  uuid: string;
  device_name: string;
  width: number;
  height: number;
  fps: number;
  bitrate_kbps: number;
  video_format: number;
  codec: string;
  hdr: boolean;
  audio_channels: number;
  state: string;
  // Real-time performance stats
  frames_sent: number;
  packets_sent: number;
  bytes_sent: number;
  idr_requests: number;
  invalidate_ref_count: number;
  client_reported_losses: number;
  encode_latency_ms: number;
  last_frame_index: number;
  uptime_seconds: number;
}

const loading = ref(false);
const rtspCount = ref(0);
const appRunning = ref(false);
const appName = ref('');
const rtspSessions = ref<RTSPSession[]>([]);
const webrtcSessions = ref<WebRTCSession[]>([]);
const showCharts = ref(false);

let pollIntervalId: ReturnType<typeof setInterval> | null = null;
const POLL_INTERVAL_MS = 2000;

const hasActiveSessions = computed(() => rtspCount.value > 0 || rtspSessions.value.length > 0 || webrtcSessions.value.length > 0);

function formatBitrate(kbps: number): string {
  if (kbps >= 1000) return `${(kbps / 1000).toFixed(1)} Mbps`;
  return `${kbps} Kbps`;
}

function formatNumber(n: number): string {
  if (n >= 1_000_000) return `${(n / 1_000_000).toFixed(1)}M`;
  if (n >= 1_000) return `${(n / 1_000).toFixed(1)}K`;
  return String(n);
}

function formatBytes(bytes: number): string {
  if (bytes >= 1_073_741_824) return `${(bytes / 1_073_741_824).toFixed(1)} GB`;
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(1)} MB`;
  if (bytes >= 1_024) return `${(bytes / 1_024).toFixed(1)} KB`;
  return `${bytes} B`;
}

function formatUptime(seconds: number): string {
  const h = Math.floor(seconds / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = Math.floor(seconds % 60);
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

async function fetchSessionStatus(): Promise<void> {
  try {
    const r = await http.get<SessionStatus>('./api/session/status', {
      validateStatus: () => true,
    });
    if (r.status === 200 && r.data) {
      rtspCount.value = r.data.activeSessions ?? 0;
      appRunning.value = r.data.appRunning ?? false;
      appName.value = r.data.appName ?? '';
    }
  } catch {
    // Silently ignore — will retry on next poll
  }
}

async function fetchRTSPSessions(): Promise<void> {
  try {
    const r = await http.get<{ sessions: RTSPSession[] }>('./api/rtsp/sessions', {
      validateStatus: () => true,
    });
    if (r.status === 200 && r.data?.sessions) {
      rtspSessions.value = r.data.sessions;
    } else {
      rtspSessions.value = [];
    }
  } catch {
    rtspSessions.value = [];
  }
}

async function fetchWebRTCSessions(): Promise<void> {
  try {
    const r = await http.get<{ sessions: WebRTCSession[] }>('./api/webrtc/sessions', {
      validateStatus: () => true,
    });
    if (r.status === 200 && r.data?.sessions) {
      webrtcSessions.value = r.data.sessions;
    } else {
      webrtcSessions.value = [];
    }
  } catch {
    webrtcSessions.value = [];
  }
}

async function refresh(): Promise<void> {
  if (!auth.isAuthenticated) return;
  loading.value = true;
  await Promise.all([fetchSessionStatus(), fetchRTSPSessions(), fetchWebRTCSessions()]);
  loading.value = false;
}

function startPolling(): void {
  if (pollIntervalId !== null) return;
  pollIntervalId = setInterval(() => {
    if (!auth.isAuthenticated) return;
    void Promise.all([fetchSessionStatus(), fetchRTSPSessions(), fetchWebRTCSessions()]);
  }, POLL_INTERVAL_MS);
}

function stopPolling(): void {
  if (pollIntervalId !== null) {
    clearInterval(pollIntervalId);
    pollIntervalId = null;
  }
}

onMounted(async () => {
  await auth.waitForAuthentication();
  await refresh();
  startPolling();
});

onBeforeUnmount(() => {
  stopPolling();
});
</script>

<style scoped>
.stat-cell {
  @apply rounded-lg bg-dark/[0.04] dark:bg-light/[0.06] px-3 py-2;
}
.stat-label {
  @apply text-[10px] uppercase tracking-wider opacity-60 font-semibold mb-0.5;
}
.stat-value {
  @apply text-sm font-mono font-semibold;
}
</style>
