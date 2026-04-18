import { defineStore } from 'pinia';
import { ref } from 'vue';
import { fetchHostInfo, fetchHostStats } from '@/services/hostStatsApi';
import { HostHistoryPoint, HostInfo, HostStatsSnapshot } from '@/types/host';

const POLL_INTERVAL_MS = 2000;
const HISTORY_WINDOW_MS = 5 * 60 * 1000;
const MAX_POINTS = Math.ceil(HISTORY_WINDOW_MS / POLL_INTERVAL_MS) + 5;

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

export const useHostStatsStore = defineStore('hostStats', () => {
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

  let intervalId = 0;
  let consumerCount = 0;
  let infoLoaded = false;

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
    const cutoff = now - HISTORY_WINDOW_MS;
    while (history.value.length && history.value[0].timestamp < cutoff) {
      history.value.shift();
    }
    if (history.value.length > MAX_POINTS) {
      history.value.splice(0, history.value.length - MAX_POINTS);
    }
  };

  const refresh = async () => {
    try {
      const s = await fetchHostStats();
      snapshot.value = s;
      pushHistory(s);
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

  const start = () => {
    consumerCount += 1;
    if (polling.value) {
      return;
    }
    polling.value = true;
    void loadInfoOnce();
    void refresh();
    intervalId = window.setInterval(() => {
      void refresh();
    }, POLL_INTERVAL_MS);
  };

  const stop = () => {
    consumerCount = Math.max(0, consumerCount - 1);
    if (consumerCount > 0) {
      return;
    }
    polling.value = false;
    if (intervalId !== 0) {
      window.clearInterval(intervalId);
      intervalId = 0;
    }
  };

  return { snapshot, info, history, lastError, polling, start, stop, refresh };
});
