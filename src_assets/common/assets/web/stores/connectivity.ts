import { defineStore } from 'pinia';
import { ref, Ref, computed } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';

export const useConnectivityStore = defineStore('connectivity', () => {
  const offline: Ref<boolean> = ref(false);
  const checking: Ref<boolean> = ref(false);
  const lastOk: Ref<number | null> = ref(null);
  const retryMs: Ref<number> = ref(15000);
  const started: Ref<boolean> = ref(false);

  let intervalId: number | null = null;
  let quickRetryTimer: number | null = null;
  let onBecameActiveHandler: ((this: Window, ev: Event) => any) | null = null;

  let failCount = 0;
  const failThreshold = 2;

  let offlineSince: number | null = null;
  const overlayDelayMs = 0;
  const quickRetryMs = 1000;

  // Utils
  const getAuth = () => {
    try {
      return useAuthStore();
    } catch {
      return null;
    }
  };
  const isLogoutInitiated = () => {
    const auth = getAuth();
    return !!(auth && (auth as any).logoutInitiated);
  };
  const isLoggingIn = () => {
    const auth = getAuth();
    return !!(auth && (auth as any).loggingIn && (auth as any).loggingIn.value === true);
  };
  const isTabActive = () => {
    try {
      const visible =
        typeof document !== 'undefined' ? document.visibilityState === 'visible' : true;
      const focus =
        typeof document !== 'undefined' && document.hasFocus ? document.hasFocus() : true;
      return visible && focus;
    } catch {
      return true;
    }
  };
  const later = (fn: () => void, ms: number) => window.setTimeout(fn, ms);

  function setOffline(v: boolean): void {
    if (isLogoutInitiated()) return; // don't show offline during intentional logout
    if (offline.value === v) return;

    if (v && !offline.value && offlineSince == null) offlineSince = Date.now();
    offline.value = v;
  }

  function refreshPage(): void {
    window.location.reload();
  }

  async function checkOnce(): Promise<void> {
    if (checking.value) return;
    checking.value = true;

    try {
      // Any HTTP status means the server is reachable.
      const res = await http.get('/api/configLocale', {
        validateStatus: () => true,
        timeout: 2500,
      });
      if (res) {
        if (quickRetryTimer) {
          clearTimeout(quickRetryTimer);
          quickRetryTimer = null;
        }
        failCount = 0;
        setOffline(false);
        lastOk.value = Date.now();
        // Handle recovery after an offline period
        if (offlineSince != null) {
          const offlineDuration = Date.now() - offlineSince;
          const reloadAfterOfflineMs = 500;
          if (offlineDuration >= reloadAfterOfflineMs) {
            const delay = offlineDuration < 200 ? 200 - offlineDuration : 0;
            later(refreshPage, delay);
          } else {
            offlineSince = null;
          }
        }
      }
    } catch {
      failCount += 1;

      // First failure: quick recheck
      if (failCount === 1 && !quickRetryTimer) {
        quickRetryTimer = later(() => {
          quickRetryTimer = null;
          if (!checking.value) checkOnce();
        }, quickRetryMs);
      } else if (failCount >= failThreshold) {
        // Confirmed offline after threshold
        setOffline(true);
      }
    } finally {
      checking.value = false;
    }
  }

  const overlayVisible = computed(() => {
    if (!offline.value || isLoggingIn()) return false;
    const since = offlineSince ?? Date.now();
    return Date.now() - since >= overlayDelayMs;
  });

  function start(): void {
    if (started.value) return;
    started.value = true;

    later(checkOnce, 500);

    intervalId = window.setInterval(() => {
      if (isTabActive()) checkOnce();
    }, retryMs.value);

    window.addEventListener('online', () => later(checkOnce, 200));
    window.addEventListener('offline', () => setOffline(true));

    onBecameActiveHandler = () =>
      later(() => {
        if (isTabActive()) checkOnce();
      }, 100);
    window.addEventListener('visibilitychange', onBecameActiveHandler);
    window.addEventListener('focus', onBecameActiveHandler);

    window.addEventListener('sunshine:offline', () => {
      /* noop: heartbeat governs */
    });
    window.addEventListener('sunshine:online', () => {
      if (isLogoutInitiated()) return;
      setOffline(false);
      lastOk.value = Date.now();
    });
  }

  function stop(): void {
    if (intervalId) {
      clearInterval(intervalId);
      intervalId = null;
    }
    if (quickRetryTimer) {
      clearTimeout(quickRetryTimer);
      quickRetryTimer = null;
    }
    if (onBecameActiveHandler) {
      try {
        window.removeEventListener('visibilitychange', onBecameActiveHandler);
        window.removeEventListener('focus', onBecameActiveHandler);
      } catch {}
      onBecameActiveHandler = null;
    }
    started.value = false;
  }

  return {
    offline,
    checking,
    lastOk,
    retryMs,
    overlayVisible,
    start,
    stop,
    checkOnce,
    refreshPage,
  };
});
