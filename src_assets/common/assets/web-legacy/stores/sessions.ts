import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import { fetchRtspSessions, fetchSessionStatus, fetchWebRtcSessions } from '@/services/sessionsApi';
import { useAuthStore } from '@/stores/auth';
import type { RTSPSession, WebRTCSession } from '@/types/sessions';

const POLL_INTERVAL_MS = 2000;
const MAX_POLL_BACKOFF_MS = 16000;

export const useSessionsStore = defineStore('sessions', () => {
  const rtspSessions = ref<RTSPSession[]>([]);
  const webrtcSessions = ref<WebRTCSession[]>([]);
  const rtspCount = ref(0);
  const appRunning = ref(false);
  const appName = ref('');
  const loading = ref(false);

  // eslint-disable-next-line @typescript-eslint/ban-types, no-restricted-syntax -- timer ID needs null sentinel for clearTimeout guard
  let pollTimerId: ReturnType<typeof setTimeout> | null = null;
  let started = false;
  let pollingConsumers = 0;
  let pollInFlight: Promise<void> | void;
  let consecutivePollFailures = 0;
  let pollingGeneration = 0;
  let latestRequestToken = 0;

  type SessionStatusResult = Awaited<ReturnType<typeof fetchSessionStatus>>;
  type RtspSessionsResult = Awaited<ReturnType<typeof fetchRtspSessions>>;
  type WebRtcSessionsResult = Awaited<ReturnType<typeof fetchWebRtcSessions>>;

  const hasActiveSessions = computed(
    () => rtspCount.value > 0 || rtspSessions.value.length > 0 || webrtcSessions.value.length > 0,
  );

  const isStreaming = computed(() => hasActiveSessions.value);

  function clearSessionState(): void {
    rtspSessions.value = [];
    webrtcSessions.value = [];
    rtspCount.value = 0;
    appRunning.value = false;
    appName.value = '';
  }

  function applySessionStatus(data: SessionStatusResult): boolean {
    if (!data) {
      return false;
    }
    rtspCount.value = data.activeSessions ?? 0;
    appRunning.value = data.appRunning ?? false;
    appName.value = data.appName ?? '';
    return true;
  }

  function applyRtspSessions(data: RtspSessionsResult): boolean {
    if (!data) {
      rtspSessions.value = [];
      return false;
    }
    rtspSessions.value = data;
    return true;
  }

  function applyWebRtcSessions(data: WebRtcSessionsResult): boolean {
    if (!data) {
      webrtcSessions.value = [];
      return false;
    }
    webrtcSessions.value = data;
    return true;
  }

  function nextPollDelayMs(): number {
    if (consecutivePollFailures <= 0) {
      return POLL_INTERVAL_MS;
    }
    return Math.min(POLL_INTERVAL_MS * 2 ** consecutivePollFailures, MAX_POLL_BACKOFF_MS);
  }

  function scheduleNextPoll(delayMs = nextPollDelayMs()): void {
    if (!started) {
      return;
    }
    if (pollTimerId !== null) {
      clearTimeout(pollTimerId);
    }
    pollTimerId = setTimeout(() => {
      pollTimerId = null;
      void poll();
    }, delayMs);
  }

  async function poll(): Promise<void> {
    if (pollInFlight) {
      return pollInFlight;
    }
    const auth = useAuthStore();
    const generation = pollingGeneration;
    if (!auth.isAuthenticated) {
      if (started && generation === pollingGeneration) {
        scheduleNextPoll(POLL_INTERVAL_MS);
      }
      return;
    }

    const requestToken = ++latestRequestToken;
    const currentPoll = (async () => {
      const [status, rtsp, webrtc] = await Promise.all([
        fetchSessionStatus(),
        fetchRtspSessions(),
        fetchWebRtcSessions(),
      ]);
      if (generation !== pollingGeneration || requestToken !== latestRequestToken) {
        return;
      }

      const results = [
        applySessionStatus(status),
        applyRtspSessions(rtsp),
        applyWebRtcSessions(webrtc),
      ];
      if (results.every(Boolean)) {
        consecutivePollFailures = 0;
      } else {
        consecutivePollFailures += 1;
      }
    })();

    pollInFlight = currentPoll;
    try {
      await currentPoll;
    } finally {
      if (pollInFlight === currentPoll) {
        pollInFlight = undefined;
      }
      if (started && generation === pollingGeneration) {
        scheduleNextPoll();
      }
    }
  }

  async function refresh(): Promise<void> {
    loading.value = true;
    await poll();
    loading.value = false;
  }

  function startPolling(): void {
    pollingConsumers += 1;
    if (started) return;
    started = true;
    consecutivePollFailures = 0;
    pollingGeneration += 1;
    latestRequestToken = 0;
    // Initial fetch
    void poll();
  }

  function stopPolling(): void {
    if (pollingConsumers > 0) {
      pollingConsumers -= 1;
    }
    if (pollingConsumers > 0) {
      return;
    }
    if (pollTimerId !== null) {
      clearTimeout(pollTimerId);
      pollTimerId = null;
    }
    started = false;
    consecutivePollFailures = 0;
    pollingGeneration += 1;
    latestRequestToken += 1;
    pollInFlight = undefined;
    clearSessionState();
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
