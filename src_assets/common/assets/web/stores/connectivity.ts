import { defineStore } from 'pinia';
import { ref, Ref, computed } from 'vue';
import { useAuthStore } from '@/stores/auth';
import { http } from '@/http';

// Connectivity heartbeat + global offline state
export const useConnectivityStore = defineStore('connectivity', () => {
  const offline: Ref<boolean> = ref(false);
  const checking: Ref<boolean> = ref(false);
  const lastOk: Ref<number | null> = ref(null);
  const retryMs: Ref<number> = ref(15000);
  const started: Ref<boolean> = ref(false);
  let intervalId: number | null = null;
  const failCount: Ref<number> = ref(0);
  const failThreshold: Ref<number> = ref(2);
  const offlineSince: Ref<number | null> = ref(null);
  const overlayDelayMs: Ref<number> = ref(0);
  const quickRetryMs: Ref<number> = ref(1000);
  let quickRetryTimer: number | null = null;

  function setOffline(v: boolean): void {
    const auth = useAuthStore();
    // Never show offline state when user intentionally logged out
    if (auth && (auth as any).logoutInitiated) return;
    if (offline.value === v) return;
    if (v && !offline.value && offlineSince.value == null) offlineSince.value = Date.now();
    offline.value = v;
  }

  function hardReload(): void {
    // Force a hard reload to fetch fresh index.html and assets.
    // Use cache-busting query to avoid serving a stale SPA shell.
    try {
      const href = window.location.href;
      const url = new URL(href);
      url.searchParams.set('_r', String(Date.now()));
      window.location.replace(url.toString());
    } catch {
      try {
        window.location.reload();
      } catch {}
    }
  }

  async function checkOnce(): Promise<void> {
    if (checking.value) return;
    checking.value = true;
    try {
      // Any HTTP response (including 401/4xx/5xx) indicates server is reachable.
      // Use an endpoint allowed while logged out.
      const res = await http.get('/api/configLocale', {
        validateStatus: () => true,
        timeout: 2500,
      });
      if (res) {
        if (quickRetryTimer) {
          clearTimeout(quickRetryTimer);
          quickRetryTimer = null;
        }
        failCount.value = 0;
        setOffline(false);
        lastOk.value = Date.now();
        // If we were offline, optionally refresh after prolonged outage only
        if (offlineSince.value != null) {
          const offlineDuration = Date.now() - offlineSince.value;
          const reloadAfterOfflineMs = 10000; // avoid hard reloads on brief blips/login
          if (offlineDuration >= reloadAfterOfflineMs) {
            const delay = offlineDuration < 200 ? 200 - offlineDuration : 0;
            setTimeout(() => hardReload(), delay);
            // Let page reload clear state
          } else {
            // Clear offline marker without reload (quick recovery)
            offlineSince.value = null;
          }
        }
      }
    } catch (e) {
      failCount.value += 1;
      if (failCount.value === 1 && !quickRetryTimer) {
        // First failure: schedule a quick verification after 1s
        quickRetryTimer = window.setTimeout(() => {
          quickRetryTimer = null;
          // Only re-check if still not checking
          if (!checking.value) checkOnce();
        }, quickRetryMs.value);
      } else if (failCount.value >= failThreshold.value) {
        // Second consecutive failure: consider truly offline
        setOffline(true);
      }
    } finally {
      checking.value = false;
    }
  }

  const overlayVisible = computed(() => {
    if (!offline.value) return false;
    // Suppress during active login attempts to avoid transient flicker
    try {
      const auth = useAuthStore();
      if (auth && (auth as any).loggingIn && (auth as any).loggingIn.value === true) return false;
    } catch {}
    const since = offlineSince.value ?? Date.now();
    return Date.now() - since >= overlayDelayMs.value;
  });

  function start(): void {
    if (started.value) return;
    started.value = true;

    // Initial check shortly after mount
    setTimeout(() => checkOnce(), 500);

    // Heartbeat interval: only ping when window is active
    intervalId = window.setInterval(() => {
      try {
        const isVisible = typeof document !== 'undefined' && document.visibilityState === 'visible';
        const hasFocus =
          typeof document !== 'undefined' && document.hasFocus ? document.hasFocus() : true;
        if (isVisible && hasFocus) checkOnce();
      } catch {
        // fallback: still attempt
        checkOnce();
      }
    }, retryMs.value);

    // Browser online/offline events reflect network, not server, but still useful
    window.addEventListener('online', () => {
      // On browser regaining network, check server reachability immediately
      setTimeout(() => checkOnce(), 200);
    });
    window.addEventListener('offline', () => setOffline(true));

    // When tab becomes active again, perform a quick check
    onBecameActiveHandler = () => {
      try {
        if (document.visibilityState === 'visible') setTimeout(() => checkOnce(), 100);
      } catch {
        setTimeout(() => checkOnce(), 100);
      }
    };
    window.addEventListener('visibilitychange', onBecameActiveHandler);
    window.addEventListener('focus', onBecameActiveHandler);

    // Listen to HTTP-layer signals for faster reaction on failures
    // We no longer react to interceptor 'offline' events; rely on heartbeat
    window.addEventListener('sunshine:offline', () => {
      // noop: heartbeat governs offline state
    });
    window.addEventListener('sunshine:online', () => {
      const auth = useAuthStore();
      if (auth && (auth as any).logoutInitiated) return;
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
    hardReload,
  };
});

// Keep reference to remove listeners on stop
let onBecameActiveHandler: ((this: Window, ev: Event) => any) | null = null;
