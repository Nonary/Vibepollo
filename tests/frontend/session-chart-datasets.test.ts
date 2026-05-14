import { describe, expect, it } from 'vitest';
import {
  buildHostComputeChartData,
  buildHostNetworkChartData,
  buildQualityChartData,
} from '@/components/session/sessionChartDatasets';
import type {
  HostSeriesField,
  SessionChartPoint,
} from '@/components/session/useSessionChartHistory';

const t = (key: string) => key;

function point(overrides: Partial<SessionChartPoint> = {}): SessionChartPoint {
  return {
    time: '10:00:00',
    encode_latency_ms: 5,
    throughput_mbps: 10,
    delta_losses: 1,
    delta_idr: 2,
    delta_invalidations: 3,
    actual_fps: 60,
    host_cpu_percent: -1,
    host_gpu_percent: -1,
    host_gpu_encoder_percent: -1,
    host_ram_percent: -1,
    host_vram_percent: -1,
    host_net_rx_bps: -1,
    host_net_tx_bps: -1,
    ...overrides,
  };
}

describe('session chart dataset builders', () => {
  it('builds RTSP quality datasets with ref invalidations', () => {
    const data = buildQualityChartData(['a'], [point()], 'rtsp', t);

    expect(data.datasets.map((dataset) => dataset.label)).toEqual([
      'sessions.client_losses',
      'sessions.idr_requests',
      'sessions.frame_invalidations',
    ]);
    expect(data.datasets.map((dataset) => dataset.data)).toEqual([[1], [2], [3]]);
  });

  it('builds WebRTC quality datasets without RTSP ref invalidations', () => {
    const data = buildQualityChartData(['a'], [point()], 'webrtc', t);

    expect(data.datasets.map((dataset) => dataset.label)).toEqual([
      'sessions.video_dropped',
      'sessions.audio_dropped',
    ]);
  });

  it('only includes available host compute series', () => {
    const available = new Set<HostSeriesField>(['host_cpu_percent', 'host_gpu_encoder_percent']);

    const data = buildHostComputeChartData(
      ['a'],
      [point({ host_cpu_percent: 25, host_gpu_encoder_percent: 50 })],
      (field) => available.has(field),
      t,
    );

    expect(data.datasets.map((dataset) => dataset.label)).toEqual([
      'sessions.chart_host_cpu',
      'sessions.chart_host_gpu_encoder',
    ]);
    expect(data.datasets.map((dataset) => dataset.data)).toEqual([[25], [50]]);
  });

  it('converts host network bps to Mbps', () => {
    const available = new Set<HostSeriesField>(['host_net_rx_bps', 'host_net_tx_bps']);

    const data = buildHostNetworkChartData(
      ['a'],
      [point({ host_net_rx_bps: 12_345_678, host_net_tx_bps: 2_500_000 })],
      (field) => available.has(field),
      t,
    );

    expect(data.datasets.map((dataset) => dataset.data)).toEqual([[12.35], [2.5]]);
  });
});
