import type {
  SessionDetail,
  SessionEvent,
  SessionSample,
  SessionSummary,
} from '../../types/sessions';

import type { PerformancePoint } from './types';

export const HISTORY_GROUP_GAP_SECONDS = 120;

export interface HistoryListPayload {
  sessions?: SessionSummary[];
  total?: number;
  total_count?: number;
  count?: number;
  history_status?: { total?: number; total_count?: number };
}

export interface HistoryPage {
  sessions: SessionSummary[];
  total: number | null;
  hasMore: boolean;
}

export interface SessionHistoryRow {
  key: string;
  summary: SessionSummary;
  members: SessionSummary[];
  isGroup: boolean;
}

export interface SessionChartPoint {
  timestamp: number;
  value: number | null;
  /** A stable stream identity keeps reconnects from becoming one line. */
  segment?: string;
}

export interface HostChartSeries {
  cpu: SessionChartPoint[];
  gpu: SessionChartPoint[];
  encoder: SessionChartPoint[];
  ram: SessionChartPoint[];
  vram: SessionChartPoint[];
  rx: SessionChartPoint[];
  tx: SessionChartPoint[];
}

function finiteNumber(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function nonNegativeNumber(value: unknown): number | null {
  const number = finiteNumber(value);
  return number == null || number < 0 ? null : number;
}

function valueOrZero(value: unknown): number {
  const number = finiteNumber(value);
  return number == null || number < 0 ? 0 : number;
}

export function parseHistoryPage(
  payload: HistoryListPayload | SessionSummary[] | null | undefined,
  pageSize: number,
): HistoryPage {
  const source = Array.isArray(payload) ? payload : payload?.sessions;
  const sessions = Array.isArray(source) ? source : [];
  const objectPayload = Array.isArray(payload) ? undefined : payload;
  const candidate =
    objectPayload?.total_count ??
    objectPayload?.total ??
    objectPayload?.count ??
    objectPayload?.history_status?.total_count ??
    objectPayload?.history_status?.total;
  const total = typeof candidate === 'number' && Number.isFinite(candidate) ? candidate : null;
  const visibleSessions = sessions.slice(0, pageSize);
  return {
    sessions: visibleSessions,
    total,
    // The native endpoint currently returns no total. A complete page means
    // another page may exist; the caller turns this into an inferred total.
    // The list request asks for one look-ahead row. This makes an exact
    // multiple of pageSize terminable without showing an empty phantom page.
    hasMore: total == null ? sessions.length > pageSize : sessions.length > 0,
  };
}

function sameIdentity(a: SessionSummary, b: SessionSummary): boolean {
  return (
    a.protocol === b.protocol &&
    a.client_name === b.client_name &&
    a.device_name === b.device_name &&
    a.app_name === b.app_name
  );
}

/**
 * Group only adjacent, chronologically reconnecting records from one client.
 * The history endpoint returns newest first, so the gap is older.start minus
 * newer.end. Unknown times cannot establish a reconnect relationship.
 */
export function groupSessionSummaries(
  sessions: SessionSummary[],
  gapSeconds = HISTORY_GROUP_GAP_SECONDS,
): SessionHistoryRow[] {
  const rows: SessionHistoryRow[] = [];
  let bucket: SessionSummary[] = [];

  const flush = () => {
    if (!bucket.length) return;
    const members = [...bucket];
    const newest = members[0];
    const oldest = members[members.length - 1];
    if (!newest || !oldest || members.length === 1) {
      const summary = members[0];
      if (summary) rows.push({ key: summary.uuid, summary, members, isGroup: false });
      bucket = [];
      return;
    }
    const worstVerdict = members.reduce(
      (worst, member) => {
        const rank = (value?: string) =>
          value === 'failed' ? 3 : value === 'degraded' ? 2 : value === 'healthy' ? 1 : 0;
        return rank(member.verdict) > rank(worst) ? member.verdict : worst;
      },
      undefined as string | undefined,
    );
    const summary: SessionSummary = {
      ...newest,
      uuid: `group:${members.map((member) => member.uuid).join('|')}`,
      start_time_unix: oldest.start_time_unix,
      end_time_unix: newest.end_time_unix,
      duration_seconds: members.reduce((sum, member) => sum + (member.duration_seconds || 0), 0),
      verdict: worstVerdict,
    };
    rows.push({ key: summary.uuid, summary, members, isGroup: true });
    bucket = [];
  };

  for (const session of sessions) {
    const previous = bucket.at(-1);
    const gap =
      previous?.start_time_unix != null && session.end_time_unix != null
        ? previous.start_time_unix - session.end_time_unix
        : null;
    if (
      previous &&
      sameIdentity(previous, session) &&
      gap != null &&
      gap >= 0 &&
      gap <= gapSeconds
    ) {
      bucket.push(session);
    } else {
      flush();
      bucket.push(session);
    }
  }
  flush();
  return rows;
}

function sessionSamples(session: SessionDetail): SessionSample[] {
  return [...(session.samples ?? [])].sort((a, b) => a.timestamp_unix - b.timestamp_unix);
}

function sessionEvents(session: SessionDetail): SessionEvent[] {
  return [...(session.events ?? [])].sort((a, b) => a.timestamp_unix - b.timestamp_unix);
}

/** Merge complete details while retaining every sample/event and its source UUID. */
export function mergeSessionDetails(details: SessionDetail[]): SessionDetail {
  const sorted = [...details].sort((a, b) => a.start_time_unix - b.start_time_unix);
  const first = sorted[0];
  if (!first) throw new Error('Cannot merge an empty session group');
  const last = sorted.at(-1) ?? first;
  const rank = (value?: string) =>
    value === 'failed' ? 3 : value === 'degraded' ? 2 : value === 'healthy' ? 1 : 0;
  const worst = sorted.reduce(
    (value, item) => (rank(item.verdict) > rank(value) ? item.verdict : value),
    undefined as string | undefined,
  );
  const samples = sorted
    .flatMap((session) =>
      sessionSamples(session).map((sample) => ({
        ...sample,
        session_uuid: sample.session_uuid || session.uuid,
      })),
    )
    .sort((a, b) => a.timestamp_unix - b.timestamp_unix);
  const events = sorted
    .flatMap((session) =>
      sessionEvents(session).map((event) => ({
        ...event,
        session_uuid: event.session_uuid || session.uuid,
      })),
    )
    .sort((a, b) => a.timestamp_unix - b.timestamp_unix);
  return {
    ...first,
    uuid: `group:${sorted.map((session) => session.uuid).join('|')}`,
    start_time_unix: first.start_time_unix,
    end_time_unix: last.end_time_unix,
    duration_seconds: sorted.reduce((sum, session) => sum + (session.duration_seconds || 0), 0),
    verdict: worst,
    requested_bitrate_kbps: Math.max(
      ...sorted.map((session) => session.requested_bitrate_kbps ?? 0),
    ),
    encoder_bitrate_kbps: Math.max(...sorted.map((session) => session.encoder_bitrate_kbps ?? 0)),
    total_samples: sorted.reduce(
      (sum, session) => sum + (session.total_samples ?? session.samples?.length ?? 0),
      0,
    ),
    total_events: sorted.reduce(
      (sum, session) => sum + (session.total_events ?? session.events?.length ?? 0),
      0,
    ),
    samples_truncated: sorted.some((session) => Boolean(session.samples_truncated)),
    events_truncated: sorted.some((session) => Boolean(session.events_truncated)),
    samples,
    events,
  };
}

function samplePoint(
  sample: SessionSample,
  value: unknown,
  transform: (value: number) => number = (number) => number,
): SessionChartPoint {
  const number = finiteNumber(value);
  return {
    timestamp: sample.timestamp_unix * 1000,
    value: number == null || number < 0 ? null : transform(number),
    segment: sample.session_uuid,
  };
}

export function samplesToHostSeries(samples: SessionSample[]): HostChartSeries {
  const ordered = [...samples].sort((a, b) => a.timestamp_unix - b.timestamp_unix);
  return {
    cpu: ordered.map((sample) => samplePoint(sample, sample.host_cpu_percent)),
    gpu: ordered.map((sample) => samplePoint(sample, sample.host_gpu_percent)),
    encoder: ordered.map((sample) => samplePoint(sample, sample.host_gpu_encoder_percent)),
    ram: ordered.map((sample) => samplePoint(sample, sample.host_ram_percent)),
    vram: ordered.map((sample) => samplePoint(sample, sample.host_vram_percent)),
    rx: ordered.map((sample) =>
      samplePoint(sample, sample.host_net_rx_bps, (value) => value / 1_000_000),
    ),
    tx: ordered.map((sample) =>
      samplePoint(sample, sample.host_net_tx_bps, (value) => value / 1_000_000),
    ),
  };
}

/** Convert persisted counters into rates/deltas without crossing reconnect boundaries. */
export function samplesToPerformancePoints(
  samples: SessionSample[],
  protocol: string,
): PerformancePoint[] {
  const ordered = [...samples].sort((a, b) => a.timestamp_unix - b.timestamp_unix);
  const isWebRtc = protocol.toLowerCase() === 'webrtc';
  return ordered.map((sample, index) => {
    const previous = ordered[index - 1];
    const sameStream = previous && (sample.session_uuid || '') === (previous.session_uuid || '');
    const quality = sameStream
      ? isWebRtc
        ? Math.max(0, sample.video_dropped - previous.video_dropped) +
          Math.max(0, sample.audio_dropped - previous.audio_dropped)
        : Math.max(0, sample.client_reported_losses - previous.client_reported_losses) +
          Math.max(0, sample.idr_requests - previous.idr_requests) +
          Math.max(0, sample.ref_invalidations - previous.ref_invalidations)
      : 0;
    const bitrateKbps = nonNegativeNumber(sample.actual_bitrate_kbps);
    return {
      timestamp: sample.timestamp_unix * 1000,
      latencyMs: nonNegativeNumber(sample.encode_latency_ms),
      throughputMbps: bitrateKbps == null ? null : bitrateKbps / 1000,
      qualityEvents: quality,
      videoDropped: sameStream ? Math.max(0, sample.video_dropped - previous.video_dropped) : 0,
      audioDropped: sameStream ? Math.max(0, sample.audio_dropped - previous.audio_dropped) : 0,
      fps: nonNegativeNumber(sample.actual_fps),
      segment: sample.session_uuid,
    };
  });
}

/** Reset counter deltas when a counter rolls back or a stream changes. */
export function counterDelta(current: number, previous: number | undefined): number {
  if (previous == null || !Number.isFinite(current) || !Number.isFinite(previous)) return 0;
  return current >= previous ? current - previous : 0;
}

export function filterChartRange<T extends { timestamp: number }>(
  points: T[],
  rangeMinutes: number | null,
  latestTimestamp = Math.max(...points.map((point) => point.timestamp), 0),
): T[] {
  if (rangeMinutes == null || rangeMinutes <= 0 || latestTimestamp <= 0) return points;
  return points.filter((point) => point.timestamp >= latestTimestamp - rangeMinutes * 60_000);
}

// Kept as a small exported helper for tests and live chart consumers.
export function nonNegative(value: unknown): number {
  return valueOrZero(value);
}
