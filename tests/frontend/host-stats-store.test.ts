import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { useConfigStore } from '@/stores/config';
import { useHostStatsStore } from '@/stores/hostStats';
import { fetchHostInfo, fetchHostStats } from '@/services/hostStatsApi';
import type { HostInfo, HostStatsSnapshot } from '@/types/host';

vi.mock('@/services/hostStatsApi', () => ({
  fetchHostInfo: vi.fn(),
  fetchHostStats: vi.fn(),
}));

const sampleSnapshot: HostStatsSnapshot = {
  cpu_percent: 12,
  cpu_temp_c: 44,
  ram_used_bytes: 1024,
  ram_total_bytes: 4096,
  ram_percent: 25,
  gpu_percent: 33,
  gpu_encoder_percent: 4,
  gpu_temp_c: 55,
  vram_used_bytes: 2048,
  vram_total_bytes: 8192,
  vram_percent: 25,
};

const sampleInfo: HostInfo = {
  cpu_model: 'Test CPU',
  gpu_model: 'Test GPU',
  cpu_logical_cores: 8,
  ram_total_bytes: 4096,
  vram_total_bytes: 8192,
};

async function flushPromises(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

function setTabState(visibilityState: DocumentVisibilityState, focused: boolean) {
  Object.defineProperty(document, 'visibilityState', {
    configurable: true,
    get: () => visibilityState,
  });
  Object.defineProperty(document, 'hidden', {
    configurable: true,
    get: () => visibilityState !== 'visible',
  });
  Object.defineProperty(document, 'hasFocus', {
    configurable: true,
    value: () => focused,
  });
}

function createStore(config: Record<string, unknown> = {}) {
  setActivePinia(createPinia());
  const configStore = useConfigStore();
  configStore.setConfig({
    realtime_stats_enabled: true,
    realtime_stats_pause_when_hidden: true,
    realtime_stats_poll_interval_ms: 2000,
    ...config,
  });
  return useHostStatsStore();
}

describe('host stats store polling', () => {
  const mockedFetchHostStats = vi.mocked(fetchHostStats);
  const mockedFetchHostInfo = vi.mocked(fetchHostInfo);
  let stores: Array<ReturnType<typeof useHostStatsStore>> = [];

  beforeEach(() => {
    vi.useFakeTimers();
    setTabState('visible', true);
    mockedFetchHostStats.mockResolvedValue(sampleSnapshot);
    mockedFetchHostInfo.mockResolvedValue(sampleInfo);
  });

  afterEach(() => {
    for (const store of stores) {
      store.stop();
    }
    stores = [];
    vi.useRealTimers();
    vi.clearAllMocks();
    setTabState('visible', true);
  });

  it('starts polling when a stats consumer mounts and stats are enabled', async () => {
    const store = createStore();
    stores.push(store);

    store.start();
    await flushPromises();

    expect(store.polling).toBe(true);
    expect(store.pausedHidden).toBe(false);
    expect(mockedFetchHostInfo).toHaveBeenCalledTimes(1);
    expect(mockedFetchHostStats).toHaveBeenCalledTimes(1);

    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(mockedFetchHostStats).toHaveBeenCalledTimes(2);
  });

  it('polls host stats without any streaming session state', async () => {
    const store = createStore();
    stores.push(store);

    store.start();
    await flushPromises();

    expect(store.polling).toBe(true);
    expect(store.snapshot.cpu_percent).toBe(sampleSnapshot.cpu_percent);
    expect(mockedFetchHostStats).toHaveBeenCalledTimes(1);
  });

  it('stops polling while the document is hidden', async () => {
    const store = createStore();
    stores.push(store);
    store.start();
    await flushPromises();

    setTabState('hidden', true);
    document.dispatchEvent(new Event('visibilitychange'));
    await flushPromises();

    expect(store.polling).toBe(false);
    expect(store.pausedHidden).toBe(true);

    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(mockedFetchHostStats).toHaveBeenCalledTimes(1);
  });

  it('stops polling while the window is unfocused', async () => {
    const store = createStore();
    stores.push(store);
    store.start();
    await flushPromises();

    setTabState('visible', false);
    window.dispatchEvent(new Event('blur'));
    await flushPromises();

    expect(store.polling).toBe(false);
    expect(store.pausedHidden).toBe(true);

    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(mockedFetchHostStats).toHaveBeenCalledTimes(1);
  });

  it('resumes with an immediate refresh when the tab becomes active again', async () => {
    const store = createStore();
    stores.push(store);
    store.start();
    await flushPromises();

    setTabState('hidden', false);
    document.dispatchEvent(new Event('visibilitychange'));
    await flushPromises();

    setTabState('visible', true);
    document.dispatchEvent(new Event('visibilitychange'));
    await flushPromises();

    expect(store.polling).toBe(true);
    expect(store.pausedHidden).toBe(false);
    expect(mockedFetchHostStats).toHaveBeenCalledTimes(2);
  });

  it('ignores inactive tabs when pause while hidden is disabled', async () => {
    setTabState('hidden', false);
    const store = createStore({ realtime_stats_pause_when_hidden: false });
    stores.push(store);

    store.start();
    await flushPromises();

    expect(store.polling).toBe(true);
    expect(store.pausedHidden).toBe(false);
    expect(mockedFetchHostStats).toHaveBeenCalledTimes(1);

    window.dispatchEvent(new Event('blur'));
    document.dispatchEvent(new Event('visibilitychange'));
    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(store.polling).toBe(true);
    expect(store.pausedHidden).toBe(false);
    expect(mockedFetchHostStats).toHaveBeenCalledTimes(2);
  });
});
