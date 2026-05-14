import type { HostSeriesField, SessionChartPoint } from './useSessionChartHistory';

type Translate = (key: string) => string;
type Protocol = 'rtsp' | 'webrtc';

interface LineDataset {
  label: string;
  data: number[];
  borderColor: string;
  backgroundColor: string;
  fill: boolean;
}

export interface LineChartData {
  labels: string[];
  datasets: LineDataset[];
}

function dataset(
  label: string,
  data: number[],
  borderColor: string,
  backgroundColor: string,
): LineDataset {
  return {
    label,
    data,
    borderColor,
    backgroundColor,
    fill: true,
  };
}

function bpsToMbps(v: number): number {
  return Math.round((v / 1_000_000) * 100) / 100;
}

export function buildLatencyChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData {
  return {
    labels,
    datasets: [
      dataset(
        t('sessions.chart_encode_latency'),
        points.map((p) => p.encode_latency_ms),
        'rgb(59, 130, 246)',
        'rgba(59, 130, 246, 0.1)',
      ),
    ],
  };
}

export function buildThroughputChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData {
  return {
    labels,
    datasets: [
      dataset(
        t('sessions.chart_throughput'),
        points.map((p) => p.throughput_mbps),
        'rgb(16, 185, 129)',
        'rgba(16, 185, 129, 0.1)',
      ),
    ],
  };
}

export function buildQualityChartData(
  labels: string[],
  points: SessionChartPoint[],
  protocol: Protocol,
  t: Translate,
): LineChartData {
  const isRtsp = protocol !== 'webrtc';
  return {
    labels,
    datasets: [
      dataset(
        isRtsp ? t('sessions.client_losses') : t('sessions.video_dropped'),
        points.map((p) => p.delta_losses),
        'rgb(239, 68, 68)',
        'rgba(239, 68, 68, 0.15)',
      ),
      dataset(
        isRtsp ? t('sessions.idr_requests') : t('sessions.audio_dropped'),
        points.map((p) => p.delta_idr),
        'rgb(245, 158, 11)',
        'rgba(245, 158, 11, 0.15)',
      ),
      ...(isRtsp
        ? [
            dataset(
              t('sessions.frame_invalidations'),
              points.map((p) => p.delta_invalidations),
              'rgb(168, 85, 247)',
              'rgba(168, 85, 247, 0.15)',
            ),
          ]
        : []),
    ],
  };
}

export function buildFpsChartData(
  labels: string[],
  points: SessionChartPoint[],
  t: Translate,
): LineChartData {
  return {
    labels,
    datasets: [
      dataset(
        t('sessions.chart_framerate'),
        points.map((p) => p.actual_fps),
        'rgb(236, 72, 153)',
        'rgba(236, 72, 153, 0.1)',
      ),
    ],
  };
}

export function buildHostComputeChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData {
  const datasets: LineDataset[] = [];
  if (hasHostSeries('host_cpu_percent')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_cpu'),
        points.map((p) => p.host_cpu_percent),
        'rgb(59, 130, 246)',
        'rgba(59, 130, 246, 0.12)',
      ),
    );
  }
  if (hasHostSeries('host_gpu_percent')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_gpu'),
        points.map((p) => p.host_gpu_percent),
        'rgb(16, 185, 129)',
        'rgba(16, 185, 129, 0.12)',
      ),
    );
  }
  if (hasHostSeries('host_gpu_encoder_percent')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_gpu_encoder'),
        points.map((p) => p.host_gpu_encoder_percent),
        'rgb(168, 85, 247)',
        'rgba(168, 85, 247, 0.12)',
      ),
    );
  }
  return { labels, datasets };
}

export function buildHostMemoryChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData {
  const datasets: LineDataset[] = [];
  if (hasHostSeries('host_ram_percent')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_ram'),
        points.map((p) => p.host_ram_percent),
        'rgb(245, 158, 11)',
        'rgba(245, 158, 11, 0.12)',
      ),
    );
  }
  if (hasHostSeries('host_vram_percent')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_vram'),
        points.map((p) => p.host_vram_percent),
        'rgb(236, 72, 153)',
        'rgba(236, 72, 153, 0.12)',
      ),
    );
  }
  return { labels, datasets };
}

export function buildHostNetworkChartData(
  labels: string[],
  points: SessionChartPoint[],
  hasHostSeries: (field: HostSeriesField) => boolean,
  t: Translate,
): LineChartData {
  const datasets: LineDataset[] = [];
  if (hasHostSeries('host_net_rx_bps')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_net_rx'),
        points.map((p) => bpsToMbps(p.host_net_rx_bps)),
        'rgb(34, 197, 94)',
        'rgba(34, 197, 94, 0.12)',
      ),
    );
  }
  if (hasHostSeries('host_net_tx_bps')) {
    datasets.push(
      dataset(
        t('sessions.chart_host_net_tx'),
        points.map((p) => bpsToMbps(p.host_net_tx_bps)),
        'rgb(59, 130, 246)',
        'rgba(59, 130, 246, 0.12)',
      ),
    );
  }
  return { labels, datasets };
}
