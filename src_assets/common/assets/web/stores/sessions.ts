import { defineStore } from 'pinia';
import { computed, ref } from 'vue';
import {
  fetchRtspSessions,
  fetchSessionStatus,
  fetchWebRtcSessions,
} from '@/services/sessionsApi';
import { useAuthStore } from '@/stores/auth';
import type { RTSPSession, SessionStatus, WebRTCSession } from '@/types/sessions';

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
  let pollInFlight: Promise<void> | null = null;
  let consecutivePollFailures = 0;

  const hasActiveSessions = computed(
    () => rtspCount.value > 0 || rtspSessions.value.length > 0 || webrtcSessions.value.length > 0,
  );

  const isStreaming = computed(() => hasActiveSessions.value);

  async function loadSessionStatus(): Promise<boolean> {
    const data: SessionStatus | null = await fetchSessionStatus();
    if (!data) {
      return false;
    }
    rtspCount.value = data.activeSessions ?? 0;
    appRunning.value = data.appRunning ?? false;
    appName.value = data.appName ?? '';
    return true;
  }

  async function loadRtspSessions(): Promise<boolean> {
    const data = await fetchRtspSessions();
    if (!data) {
      rtspSessions.value = [];
      return false;
    }
    rtspSessions.value = data;
    return true;
  }

  async function loadWebRtcSessions(): Promise<boolean> {
    const data = await fetchWebRtcSessions();
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
    if (!auth.isAuthenticated) {
      scheduleNextPoll(POLL_INTERVAL_MS);
      return;
    }
    pollInFlight = (async () => {
      const results = await Promise.all([
        loadSessionStatus(),
        loadRtspSessions(),
        loadWebRtcSessions(),
      ]);
      if (results.every(Boolean)) {
        consecutivePollFailures = 0;
      } else {
        consecutivePollFailures += 1;
      }
    })();
    try {
      await pollInFlight;
    } finally {
      pollInFlight = null;
      scheduleNextPoll();
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
    pollInFlight = null;
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
