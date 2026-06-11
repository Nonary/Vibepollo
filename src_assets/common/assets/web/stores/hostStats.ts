import { defineStore } from 'pinia';
import { computed, ref, watch } from 'vue';
import { fetchHostInfo, fetchHostStats } from '@/services/hostStatsApi';
import { useConfigStore } from '@/stores/config';
import { HostHistoryPoint, HostInfo, HostStatsSnapshot } from '@/types/host';

const DEFAULT_POLL_INTERVAL_MS = 2000;
const DEFAULT_HISTORY_WINDOW_S = 300;
const DEFAULT_MAX_POINTS = 300;

const emptySnapshot: HostStatsSnapshot = {
  cpu_percent: -1,
  cpu_temp_c: -1,
  ram_used_bytes: 0,
  ram_total_bytes: 0,
  ram_percent: 0,
  gpu_percent: -1,
  gpu_encoder_percent: -1,
  gpu_temp_c: -1,
  vram_used_bytes: 0,
  vram_total_bytes: 0,
  vram_percent: 0,
};

function clampNumber(value: unknown, fallback: number, min: number, max: number): number {
  const n = Number(value);
  if (!Number.isFinite(n)) return fallback;
  return Math.min(max, Math.max(min, n));
}

function isTabActive(): boolean {
  if (typeof document === 'undefined') {
    return true;
  }

  const visible =
    typeof document.visibilityState === 'string'
      ? document.visibilityState === 'visible'
      : !document.hidden;
  const focused = typeof document.hasFocus === 'function' ? document.hasFocus() : true;

  return visible && focused;
}

export const useHostStatsStore = defineStore('hostStats', () => {
  const configStore = useConfigStore();

  const snapshot = ref<HostStatsSnapshot>({ ...emptySnapshot });
  const info = ref<HostInfo>({
    cpu_model: '',
    gpu_model: '',
    cpu_logical_cores: 0,
    ram_total_bytes: 0,
    vram_total_bytes: 0,
  });
  const history = ref<HostHistoryPoint[]>([]);
  const lastError = ref<string>('');
  const polling = ref<boolean>(false);
  const pausedHidden = ref<boolean>(false);
  const lastUpdated = ref<number>();

  let intervalId = 0;
  let consumerCount = 0;
  let infoLoaded = false;
  let visibilityHooked = false;

  // Stats preferences sourced from the shared server config.
  const statsEnabled = computed(() => Boolean(configStore.config?.realtime_stats_enabled ?? true));
  const pollIntervalMs = computed(() =>
    clampNumber(
      configStore.config?.realtime_stats_poll_interval_ms,
      DEFAULT_POLL_INTERVAL_MS,
      250,
      60000,
    ),
  );
  const historyWindowMs = computed(
    () =>
      clampNumber(
        configStore.config?.realtime_stats_history_retention_seconds,
        DEFAULT_HISTORY_WINDOW_S,
        30,
        3600,
      ) * 1000,
  );
  const maxPoints = computed(() =>
    clampNumber(
      configStore.config?.realtime_stats_max_history_points,
      DEFAULT_MAX_POINTS,
      30,
      2000,
    ),
  );
  const pauseWhenHidden = computed(() =>
    Boolean(configStore.config?.realtime_stats_pause_when_hidden ?? true),
  );

  const pushHistory = (s: HostStatsSnapshot) => {
    const now = Date.now();
    history.value.push({
      timestamp: now,
      cpu_percent: s.cpu_percent,
      gpu_percent: s.gpu_percent,
      gpu_encoder_percent: s.gpu_encoder_percent,
      ram_percent: s.ram_percent,
      vram_percent: s.vram_percent,
    });
    trimHistory();
  };

  const trimHistory = () => {
    const cutoff = Date.now() - historyWindowMs.value;
    while (history.value.length > 0 && (history.value[0]?.timestamp ?? Infinity) < cutoff) {
      history.value.shift();
    }
    if (history.value.length > maxPoints.value) {
      history.value.splice(0, history.value.length - maxPoints.value);
    }
  };

  const refresh = async () => {
    if (!statsEnabled.value) {
      return;
    }
    try {
      const s = await fetchHostStats();
      snapshot.value = s;
      pushHistory(s);
      lastUpdated.value = Date.now();
      lastError.value = '';
    } catch (err: unknown) {
      lastError.value = err instanceof Error ? err.message : 'host_stats fetch failed';
    }
  };

  const loadInfoOnce = async () => {
    if (infoLoaded) {
      return;
    }
    try {
      info.value = await fetchHostInfo();
      infoLoaded = true;
    } catch (err: unknown) {
      lastError.value = err instanceof Error ? err.message : 'host_info fetch failed';
    }
  };

  const clearTimer = () => {
    if (intervalId !== 0) {
      window.clearInterval(intervalId);
      intervalId = 0;
    }
  };

  const shouldPoll = () =>
    consumerCount > 0 && statsEnabled.value && !(pauseWhenHidden.value && !isTabActive());

  const syncTimer = (immediate: boolean) => {
    clearTimer();
    pausedHidden.value = consumerCount > 0 && pauseWhenHidden.value && !isTabActive();
    if (!shouldPoll()) {
      polling.value = false;
      return;
    }
    polling.value = true;
    void loadInfoOnce();
    if (immediate) {
      void refresh();
    }
    intervalId = window.setInterval(() => {
      void refresh();
    }, pollIntervalMs.value);
  };

  const onActivityChange = () => {
    syncTimer(isTabActive());
  };

  const hookVisibility = () => {
    if (visibilityHooked || typeof document === 'undefined') return;
    document.addEventListener('visibilitychange', onActivityChange);
    if (typeof window !== 'undefined') {
      window.addEventListener('focus', onActivityChange);
      window.addEventListener('blur', onActivityChange);
    }
    visibilityHooked = true;
  };

  const start = () => {
    consumerCount += 1;
    hookVisibility();
    if (consumerCount === 1 || !polling.value) {
      syncTimer(true);
    }
  };

  const stop = () => {
    consumerCount = Math.max(0, consumerCount - 1);
    if (consumerCount > 0) {
      return;
    }
    polling.value = false;
    pausedHidden.value = false;
    clearTimer();
  };

  // React to settings changes while polling is active.
  watch([statsEnabled, pollIntervalMs, pauseWhenHidden], () => {
    if (consumerCount > 0) {
      syncTimer(statsEnabled.value);
    }
    if (!statsEnabled.value) {
      snapshot.value = { ...emptySnapshot };
      history.value = [];
      lastUpdated.value = undefined;
    }
  });

  watch([historyWindowMs, maxPoints], () => {
    trimHistory();
  });

  return {
    snapshot,
    info,
    history,
    lastError,
    polling,
    pausedHidden,
    lastUpdated,
    statsEnabled,
    pollIntervalMs,
    start,
    stop,
    refresh,
  };
});
