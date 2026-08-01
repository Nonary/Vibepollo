import type { SessionEvent, SessionSample } from '../../types/sessions';

export interface ReadonlyValue<T> {
  readonly value: T;
}

export interface SessionSnapshot {
  fps?: number;
  encode_latency_ms?: number;
  frames_sent?: number;
  packets_sent?: number;
  bytes_sent?: number;
  client_reported_losses?: number;
  idr_requests?: number;
  invalidate_ref_count?: number;
  video_packets?: number;
  audio_packets?: number;
  video_dropped?: number;
  audio_dropped?: number;
  last_video_frame_index?: number;
}

export interface SessionChartPoint {
  timestamp_unix: number;
  time: string;
  encode_latency_ms: number;
  throughput_mbps: number;
  delta_losses: number;
  delta_idr: number;
  delta_invalidations: number;
  actual_fps: number;
  host_cpu_percent: number;
  host_gpu_percent: number;
  host_gpu_encoder_percent: number;
  host_ram_percent: number;
  host_vram_percent: number;
  host_net_rx_bps: number;
  host_net_tx_bps: number;
}

interface SessionChartHistoryProps {
  session?: SessionSnapshot;
  sessionId?: string;
  protocol?: 'rtsp' | 'webrtc';
  mode?: 'live' | 'history';
  historyData?: SessionSample[];
  events?: SessionEvent[];
  windowMinutes?: number | null;
  locale?: string;
}

export type HostSeriesField = keyof Pick<
  SessionSample,
  | 'host_cpu_percent'
  | 'host_gpu_percent'
  | 'host_gpu_encoder_percent'
  | 'host_ram_percent'
  | 'host_vram_percent'
  | 'host_net_rx_bps'
  | 'host_net_tx_bps'
>;

interface SessionChartHistory {
  displayData: ReadonlyValue<SessionChartPoint[]>;
  labels: ReadonlyValue<string[]>;
  hasHostSeries: (field: HostSeriesField) => boolean;
  hasHostCompute: ReadonlyValue<boolean>;
  hasHostMemory: ReadonlyValue<boolean>;
  hasHostNetwork: ReadonlyValue<boolean>;
  eventAnnotations: ReadonlyValue<Record<string, unknown>>;
}

export declare function useSessionChartHistory(props: SessionChartHistoryProps): SessionChartHistory;
