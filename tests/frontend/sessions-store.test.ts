import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest';
import { createPinia, setActivePinia } from 'pinia';
import { useSessionsStore } from '@/stores/sessions';
import { useAuthStore } from '@/stores/auth';
import { fetchRtspSessions, fetchSessionStatus, fetchWebRtcSessions } from '@/services/sessionsApi';
import type { RTSPSession, SessionStatus, WebRTCSession } from '@/types/sessions';

vi.mock('@/stores/auth', () => ({
  useAuthStore: vi.fn(),
}));

vi.mock('@/services/sessionsApi', () => ({
  fetchSessionStatus: vi.fn(),
  fetchRtspSessions: vi.fn(),
  fetchWebRtcSessions: vi.fn(),
}));

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((res) => {
    resolve = res;
  });
  return { promise, resolve };
}

async function flushPromises(): Promise<void> {
  await Promise.resolve();
  await Promise.resolve();
}

describe('sessions store polling', () => {
  const mockedUseAuthStore = vi.mocked(useAuthStore);
  const mockedFetchSessionStatus = vi.mocked(fetchSessionStatus);
  const mockedFetchRtspSessions = vi.mocked(fetchRtspSessions);
  const mockedFetchWebRtcSessions = vi.mocked(fetchWebRtcSessions);

  beforeEach(() => {
    setActivePinia(createPinia());
    vi.useFakeTimers();

    mockedUseAuthStore.mockReturnValue({
      isAuthenticated: true,
    } as any);

    mockedFetchSessionStatus.mockResolvedValue({
      activeSessions: 1,
      appRunning: true,
      appName: 'Initial App',
      paused: false,
      status: true,
    } satisfies SessionStatus);
    mockedFetchRtspSessions.mockResolvedValue([] satisfies RTSPSession[]);
    mockedFetchWebRtcSessions.mockResolvedValue([] satisfies WebRTCSession[]);
  });

  afterEach(() => {
    vi.useRealTimers();
    vi.clearAllMocks();
  });

  it('keeps polling until the last consumer stops', async () => {
    const store = useSessionsStore();

    store.startPolling();
    store.startPolling();
    await flushPromises();

    expect(mockedFetchSessionStatus).toHaveBeenCalledTimes(1);

    store.stopPolling();
    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(mockedFetchSessionStatus).toHaveBeenCalledTimes(2);

    store.stopPolling();
    await vi.advanceTimersByTimeAsync(2000);
    await flushPromises();

    expect(mockedFetchSessionStatus).toHaveBeenCalledTimes(2);
  });

  it('ignores stale responses after polling restarts', async () => {
    const firstStatus = deferred<SessionStatus | null>();
    const firstRtsp = deferred<RTSPSession[] | null>();
    const firstWebRtc = deferred<WebRTCSession[] | null>();

    mockedFetchSessionStatus
      .mockImplementationOnce(() => firstStatus.promise)
      .mockResolvedValueOnce({
        activeSessions: 2,
        appRunning: true,
        appName: 'Fresh App',
        paused: false,
        status: true,
      } satisfies SessionStatus);
    mockedFetchRtspSessions
      .mockImplementationOnce(() => firstRtsp.promise)
      .mockResolvedValueOnce([
        {
          uuid: 'fresh-rtsp',
          device_name: 'Fresh Client',
          state: 'running',
          width: 1920,
          height: 1080,
          fps: 60,
          encoder_bitrate_kbps: 20000,
          requested_bitrate_kbps: 22000,
          video_format: 0,
          codec: 'H.264',
          hdr: false,
          yuv444: false,
          audio_channels: 2,
          encode_latency_ms: 4,
          frames_sent: 10,
          packets_sent: 10,
          bytes_sent: 1000,
          client_reported_losses: 0,
          idr_requests: 0,
          invalidate_ref_count: 0,
          last_frame_index: 10,
          uptime_seconds: 5,
        },
      ] satisfies RTSPSession[]);
    mockedFetchWebRtcSessions
      .mockImplementationOnce(() => firstWebRtc.promise)
      .mockResolvedValueOnce([] satisfies WebRTCSession[]);

    const store = useSessionsStore();
    store.startPolling();
    await flushPromises();

    store.stopPolling();
    store.startPolling();
    await flushPromises();
    await flushPromises();

    expect(store.appName).toBe('Fresh App');
    expect(store.rtspSessions).toHaveLength(1);
    expect(store.rtspSessions[0]?.uuid).toBe('fresh-rtsp');

    firstStatus.resolve({
      activeSessions: 9,
      appRunning: true,
      appName: 'Stale App',
      paused: false,
      status: true,
    });
    firstRtsp.resolve([
      {
        uuid: 'stale-rtsp',
        device_name: 'Stale Client',
        state: 'running',
        width: 1280,
        height: 720,
        fps: 30,
        encoder_bitrate_kbps: 10000,
        requested_bitrate_kbps: 12000,
        video_format: 1,
        codec: 'HEVC',
        hdr: false,
        yuv444: false,
        audio_channels: 2,
        encode_latency_ms: 9,
        frames_sent: 99,
        packets_sent: 99,
        bytes_sent: 9000,
        client_reported_losses: 9,
        idr_requests: 1,
        invalidate_ref_count: 1,
        last_frame_index: 99,
        uptime_seconds: 9,
      },
    ]);
    firstWebRtc.resolve([]);
    await flushPromises();
    await flushPromises();

    expect(store.appName).toBe('Fresh App');
    expect(store.rtspSessions).toHaveLength(1);
    expect(store.rtspSessions[0]?.uuid).toBe('fresh-rtsp');
  });
});
