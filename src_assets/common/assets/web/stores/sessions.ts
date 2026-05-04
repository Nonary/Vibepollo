import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import { http } from '@/http';
import { useAuthStore } from '@/stores/auth';

export interface SessionStatus {
  activeSessions: number;
  appRunning: boolean;
  appName: string;
  paused: boolean;
  status: boolean;
}

export interface RTSPSession {
  uuid: string;
  device_name: string;
  width: number;
  height: number;
  fps: number;
  bitrate_kbps: number;
  client_bitrate_kbps?: number;
  video_format: number;
  codec: string;
  hdr: boolean;
  audio_channels: number;
  state: string;
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

export interface WebRTCSession {
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
  width?: number;
  height?: number;
  fps?: number;
  bitrate_kbps?: number;
  client_bitrate_kbps?: number;
  codec?: string;
  hdr?: boolean;
  audio_channels?: number;
  audio_codec?: string;
  profile?: string;
  video_pacing_mode?: string;
  video_pacing_slack_ms?: number;
  video_max_frame_age_ms?: number;
  video_bytes_total: number;
  audio_bytes_total: number;
  bytes_sent: number;
  last_audio_bytes: number;
  last_video_bytes: number;
  last_video_idr: boolean;
  last_video_frame_index: number;
  last_audio_age_ms?: number;
  last_video_age_ms?: number;
}

const POLL_INTERVAL_MS = 2000;

export const useSessionsStore = defineStore('sessions', () => {
  const rtspSessions = ref<RTSPSession[]>([]);
  const webrtcSessions = ref<WebRTCSession[]>([]);
  const rtspCount = ref(0);
  const appRunning = ref(false);
  const appName = ref('');
  const loading = ref(false);

  // eslint-disable-next-line @typescript-eslint/ban-types, no-restricted-syntax -- timer ID needs null sentinel for clearInterval guard
  let pollIntervalId: ReturnType<typeof setInterval> | null = null;
  let started = false;

  const hasActiveSessions = computed(
    () => rtspCount.value > 0 || rtspSessions.value.length > 0 || webrtcSessions.value.length > 0,
  );

  const isStreaming = computed(() => hasActiveSessions.value);

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

  async function poll(): Promise<void> {
    const auth = useAuthStore();
    if (!auth.isAuthenticated) return;
    await Promise.all([fetchSessionStatus(), fetchRTSPSessions(), fetchWebRTCSessions()]);
  }

  async function refresh(): Promise<void> {
    loading.value = true;
    await poll();
    loading.value = false;
  }

  function startPolling(): void {
    if (started) return;
    started = true;
    // Initial fetch
    void poll();
    pollIntervalId = setInterval(() => void poll(), POLL_INTERVAL_MS);
  }

  function stopPolling(): void {
    if (pollIntervalId !== null) {
      clearInterval(pollIntervalId);
      pollIntervalId = null;
    }
    started = false;
    rtspSessions.value = [];
    webrtcSessions.value = [];
    rtspCount.value = 0;
    appRunning.value = false;
    appName.value = '';
  }

  return {
    rtspSessions,
    webrtcSessions,
    rtspCount,
    appRunning,
    appName,
    loading,
    hasActiveSessions,
    isStreaming,
    refresh,
    startPolling,
    stopPolling,
  };
});
