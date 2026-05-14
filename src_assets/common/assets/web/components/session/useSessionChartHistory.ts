import { computed, onBeforeUnmount, ref, watch } from 'vue';
import type { SessionEvent, SessionSample } from '@/types/sessions';

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

const MAX_POINTS = 150;

const eventColors: Record<string, string> = {
  stream_started: 'rgba(34, 197, 94, 0.6)',
  stream_ended: 'rgba(59, 130, 246, 0.6)',
  first_drop: 'rgba(239, 68, 68, 0.7)',
  drop_burst: 'rgba(239, 68, 68, 0.5)',
  stall: 'rgba(245, 158, 11, 0.7)',
  recovery: 'rgba(34, 197, 94, 0.5)',
};

function nz(v?: number): number {
  if (typeof v !== 'number') return 0;
  return v < 0 ? 0 : v;
}

function formatTime(date: Date): string {
  return date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
}

function convertHistoryData(samples: SessionSample[]): SessionChartPoint[] {
  if (!samples.length) return [];
  return samples.map((sample, i) => {
    const prev = i > 0 ? samples[i - 1] : undefined;
    return {
      time: formatTime(new Date(sample.timestamp_unix * 1000)),
      encode_latency_ms: sample.encode_latency_ms,
      throughput_mbps: Math.round((sample.actual_bitrate_kbps / 1000) * 100) / 100,
      delta_losses: prev
        ? Math.max(0, sample.client_reported_losses - prev.client_reported_losses)
        : 0,
      delta_idr: prev ? Math.max(0, sample.idr_requests - prev.idr_requests) : 0,
      delta_invalidations: prev
        ? Math.max(0, sample.ref_invalidations - prev.ref_invalidations)
        : 0,
      actual_fps: Math.round(sample.actual_fps * 10) / 10,
      host_cpu_percent: nz(sample.host_cpu_percent),
      host_gpu_percent: nz(sample.host_gpu_percent),
      host_gpu_encoder_percent: nz(sample.host_gpu_encoder_percent),
      host_ram_percent: nz(sample.host_ram_percent),
      host_vram_percent: nz(sample.host_vram_percent),
      host_net_rx_bps: nz(sample.host_net_rx_bps),
      host_net_tx_bps: nz(sample.host_net_tx_bps),
    };
  });
}

function findClosestSampleIndex(samples: SessionSample[], timestampUnix: number): number {
  if (!samples.length) {
    return 0;
  }

  let low = 0;
  let high = samples.length - 1;

  while (low < high) {
    const mid = Math.floor((low + high) / 2);
    const midTimestamp = samples[mid]?.timestamp_unix ?? 0;
    if (midTimestamp < timestampUnix) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }

  const upperIndex = low;
  if (upperIndex <= 0) {
    return 0;
  }

  const lowerIndex = upperIndex - 1;
  const lowerDiff = Math.abs(timestampUnix - (samples[lowerIndex]?.timestamp_unix ?? 0));
  const upperDiff = Math.abs(timestampUnix - (samples[upperIndex]?.timestamp_unix ?? 0));
  return lowerDiff <= upperDiff ? lowerIndex : upperIndex;
}

export function useSessionChartHistory(props: SessionChartHistoryProps) {
  const history = ref<SessionChartPoint[]>([]);
  let prevSnapshot: SessionSnapshot | void;
  let prevTimestamp = 0;
  let trackedSessionId: string | void;

  function resetHistory(): void {
    history.value = [];
    prevSnapshot = undefined;
    prevTimestamp = 0;
  }

  function hasHostSeries(field: HostSeriesField): boolean {
    if (props.mode !== 'history' || !props.historyData?.length) return false;
    return props.historyData.some((sample) => {
      const value = sample[field];
      return typeof value === 'number' && value >= 0;
    });
  }

  const hasHostCompute = computed(
    () =>
      hasHostSeries('host_cpu_percent') ||
      hasHostSeries('host_gpu_percent') ||
      hasHostSeries('host_gpu_encoder_percent'),
  );
  const hasHostMemory = computed(
    () => hasHostSeries('host_ram_percent') || hasHostSeries('host_vram_percent'),
  );
  const hasHostNetwork = computed(
    () => hasHostSeries('host_net_rx_bps') || hasHostSeries('host_net_tx_bps'),
  );

  const displayData = computed<SessionChartPoint[]>(() => {
    if (props.mode === 'history' && props.historyData) {
      return convertHistoryData(props.historyData);
    }
    return history.value;
  });

  watch(
    () => props.session,
    (session) => {
      if (props.mode === 'history' || !session) {
        return;
      }

      const currentId = props.sessionId;
      if (currentId !== trackedSessionId) {
        resetHistory();
        trackedSessionId = currentId;
        prevSnapshot = { ...session };
        prevTimestamp = Date.now();
        return;
      }

      const now = Date.now();
      const isRtsp = props.protocol !== 'webrtc';
      let throughput_mbps = 0;
      let delta_losses = 0;
      let delta_idr = 0;
      let delta_invalidations = 0;
      let actual_fps = 0;

      if (prevSnapshot && prevTimestamp > 0) {
        const dt = (now - prevTimestamp) / 1000;
        if (dt > 0) {
          if (isRtsp) {
            const deltaBytes = (session.bytes_sent ?? 0) - (prevSnapshot.bytes_sent ?? 0);
            throughput_mbps = Math.max(0, (deltaBytes * 8) / (dt * 1_000_000));
            delta_losses = Math.max(
              0,
              (session.client_reported_losses ?? 0) - (prevSnapshot.client_reported_losses ?? 0),
            );
            delta_idr = Math.max(0, (session.idr_requests ?? 0) - (prevSnapshot.idr_requests ?? 0));
            delta_invalidations = Math.max(
              0,
              (session.invalidate_ref_count ?? 0) - (prevSnapshot.invalidate_ref_count ?? 0),
            );
            const deltaFrames = (session.frames_sent ?? 0) - (prevSnapshot.frames_sent ?? 0);
            actual_fps = Math.max(0, deltaFrames / dt);
          } else {
            const deltaBytes = (session.bytes_sent ?? 0) - (prevSnapshot.bytes_sent ?? 0);
            throughput_mbps = Math.max(0, (deltaBytes * 8) / (dt * 1_000_000));
            delta_losses = Math.max(
              0,
              (session.video_dropped ?? 0) - (prevSnapshot.video_dropped ?? 0),
            );
            delta_idr = Math.max(
              0,
              (session.audio_dropped ?? 0) - (prevSnapshot.audio_dropped ?? 0),
            );
            const prevFrame = prevSnapshot.last_video_frame_index ?? 0;
            const curFrame = session.last_video_frame_index ?? 0;
            actual_fps = Math.max(0, (curFrame - prevFrame) / dt);
          }
        }
      }

      const point: SessionChartPoint = {
        time: formatTime(new Date()),
        encode_latency_ms: session.encode_latency_ms ?? 0,
        throughput_mbps: Math.round(throughput_mbps * 100) / 100,
        delta_losses,
        delta_idr,
        delta_invalidations,
        actual_fps: Math.round(actual_fps * 10) / 10,
        host_cpu_percent: 0,
        host_gpu_percent: 0,
        host_gpu_encoder_percent: 0,
        host_ram_percent: 0,
        host_vram_percent: 0,
        host_net_rx_bps: 0,
        host_net_tx_bps: 0,
      };

      const nextHistory = [...history.value, point];
      if (nextHistory.length > MAX_POINTS) {
        nextHistory.shift();
      }
      history.value = nextHistory;

      prevSnapshot = { ...session };
      prevTimestamp = now;
    },
    { deep: true },
  );

  onBeforeUnmount(() => {
    resetHistory();
    trackedSessionId = undefined;
  });

  const labels = computed(() => displayData.value.map((point) => point.time));

  const eventAnnotations = computed(() => {
    if (props.mode !== 'history' || !props.events?.length || !props.historyData?.length) {
      return {};
    }
    if (!displayData.value.length) {
      return {};
    }

    const resolved = props.events.map((event, index) => ({
      index,
      event,
      closest: findClosestSampleIndex(props.historyData ?? [], event.timestamp_unix),
    }));

    const ordered = [...resolved].sort((a, b) => a.closest - b.closest);
    const clusterWindow = 3;
    const labelDecisions = new Map<number, { show: boolean; position: 'start' | 'end' }>();
    let lastIndex = -Infinity;
    let runLength = 0;
    let alternate: 'start' | 'end' = 'start';

    for (const item of ordered) {
      if (item.closest - lastIndex <= clusterWindow) {
        runLength += 1;
        alternate = alternate === 'start' ? 'end' : 'start';
      } else {
        runLength = 1;
        alternate = 'start';
      }
      labelDecisions.set(item.index, { show: runLength <= 2, position: alternate });
      lastIndex = item.closest;
    }

    const annotations: Record<string, unknown> = {};
    for (const { index, event, closest } of resolved) {
      const decision = labelDecisions.get(index) ?? { show: true, position: 'start' as const };
      annotations[`event-${index}`] = {
        type: 'line',
        xMin: closest,
        xMax: closest,
        borderColor: eventColors[event.event_type] ?? 'rgba(128, 128, 128, 0.5)',
        borderWidth: 2,
        borderDash: [4, 4],
        label: {
          display: decision.show,
          content: event.event_type.replace(/_/g, ' '),
          position: decision.position,
          backgroundColor: eventColors[event.event_type] ?? 'rgba(128, 128, 128, 0.8)',
          color: '#fff',
          font: { size: 9 },
          padding: 3,
        },
      };
    }
    return annotations;
  });

  return {
    displayData,
    labels,
    hasHostSeries,
    hasHostCompute,
    hasHostMemory,
    hasHostNetwork,
    eventAnnotations,
  };
}
