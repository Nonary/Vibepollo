import assert from 'node:assert/strict';
import test from 'node:test';

import {
  counterDelta,
  filterChartRange,
  groupSessionSummaries,
  mergeSessionDetails,
  parseHistoryPage,
  samplesToPerformancePoints,
} from '../components/stats/historyUtils.ts';
import { eventsForChart, eventTimestampMs } from '../components/stats/eventUtils.ts';
import {
  clampChartRange,
  panChartRange,
  zoomChartRange,
} from '../components/stats/chartViewport.ts';
import { downsampleHostHistory, hostHistoryPeaks } from '../utils/v2Parity.ts';

function summary(uuid: string, start: number, end: number, extra: Record<string, unknown> = {}) {
  return {
    uuid,
    protocol: 'rtsp',
    client_name: 'Moonlight',
    device_name: 'Living room',
    app_name: 'Portal',
    codec: 'h264',
    width: 1920,
    height: 1080,
    target_fps: 60,
    encoder_bitrate_kbps: 20_000,
    audio_channels: 2,
    hdr: false,
    start_time_unix: start,
    end_time_unix: end,
    duration_seconds: end - start,
    ...extra,
  };
}

function sample(uuid: string, timestamp: number, bytes: number, losses = 0) {
  return {
    session_uuid: uuid,
    timestamp_unix: timestamp,
    bytes_sent_total: bytes,
    packets_sent_video: bytes,
    frames_sent: bytes,
    last_frame_index: bytes,
    video_dropped: losses,
    audio_dropped: losses,
    client_reported_losses: losses,
    idr_requests: losses,
    ref_invalidations: losses,
    encode_latency_ms: 4,
    actual_fps: 60,
    actual_bitrate_kbps: 20_000,
    frame_interval_jitter_ms: 1,
    host_cpu_percent: 20,
    host_gpu_percent: 30,
    host_gpu_encoder_percent: 40,
    host_net_rx_bps: 2_000_000,
    host_net_tx_bps: 4_000_000,
  };
}

test('reconnect grouping requires protocol, client, device, app, and a bounded gap', () => {
  const rows = groupSessionSummaries([
    summary('new', 1_300, 1_360),
    summary('old', 1_180, 1_240),
    summary('other-device', 1_050, 1_110, { device_name: 'Bedroom' }),
    summary('other-app', 900, 960, { app_name: 'Doom' }),
    summary('far-away', 500, 560),
  ]);
  assert.equal(rows[0]?.isGroup, true);
  assert.deepEqual(
    rows[0]?.members.map((member) => member.uuid),
    ['new', 'old'],
  );
  assert.equal(rows[1]?.isGroup, false);
  assert.equal(rows[2]?.isGroup, false);
  assert.equal(rows[3]?.isGroup, false);
});

test('look-ahead pagination reaches the end without an empty phantom page', () => {
  const pageSize = 12;
  const first = parseHistoryPage(
    {
      sessions: Array.from({ length: pageSize + 1 }, (_, index) =>
        summary(`s${index}`, index, index + 1),
      ),
    },
    pageSize,
  );
  const final = parseHistoryPage({ sessions: [summary('s12', 12, 13)] }, pageSize);
  assert.equal(first.sessions.length, pageSize);
  assert.equal(first.hasMore, true);
  assert.equal(final.sessions.length, 1);
  assert.equal(final.hasMore, false);
});

test('merged reconnect details retain chronological samples/events and source segments', () => {
  const first = {
    ...summary('first', 1_000, 1_060),
    samples: [sample('first', 1_020, 100), sample('first', 1_050, 200)],
    events: [{ session_uuid: 'first', timestamp_unix: 1_030, event_type: 'stall', payload: '' }],
  };
  const second = {
    ...summary('second', 1_100, 1_140),
    samples: [sample('second', 1_110, 300)],
    events: [
      { session_uuid: 'second', timestamp_unix: 1_120, event_type: 'recovery', payload: '' },
    ],
  };
  const merged = mergeSessionDetails([second, first] as never[]);
  assert.deepEqual(
    merged.samples.map((point) => point.timestamp_unix),
    [1_020, 1_050, 1_110],
  );
  assert.deepEqual(
    merged.events.map((event) => event.event_type),
    ['stall', 'recovery'],
  );
  const points = samplesToPerformancePoints(merged.samples, 'rtsp');
  assert.equal(points[0]?.qualityEvents, 0);
  assert.equal(points[2]?.qualityEvents, 0, 'deltas must reset at reconnect boundaries');
  assert.deepEqual(
    points.map((point) => point.segment),
    ['first', 'first', 'second'],
  );
});

test('range filtering keeps real timestamps and missing samples', () => {
  const points = [
    { timestamp: 100_000, value: 1 },
    { timestamp: 160_000, value: null },
    { timestamp: 220_000, value: 3 },
  ];
  assert.deepEqual(filterChartRange(points, 1, 220_000), points.slice(1));
  assert.deepEqual(filterChartRange(points, null, 220_000), points);
});

test('counter deltas never turn a reset into a large positive spike', () => {
  assert.equal(counterDelta(12, 10), 2);
  assert.equal(counterDelta(2, 10), 0);
  assert.equal(counterDelta(Number.NaN, 10), 0);
});

test('event seconds are converted once and remain aligned to irregular millisecond samples', () => {
  assert.equal(eventTimestampMs(1_700_000_123), 1_700_000_123_000);
  assert.equal(eventTimestampMs(1_700_000_123_000), 1_700_000_123_000);
  const points = [
    { timestamp: 1_700_000_000_000, value: 1, segment: 'first' },
    { timestamp: 1_700_000_002_000, value: 2, segment: 'first' },
    { timestamp: 1_700_000_020_000, value: 3, segment: 'first' },
  ];
  const events = eventsForChart(
    [
      {
        session_uuid: 'first',
        timestamp_unix: 1_700_000_002,
        event_type: 'stall',
        payload: 'two seconds',
      },
      {
        session_uuid: 'first',
        timestamp_unix: 1_700_000_100,
        event_type: 'outside-gap',
        payload: '',
      },
      {
        session_uuid: 'first',
        timestamp_unix: 1_700_000_025,
        event_type: 'stream_ended',
        payload: 'five seconds after last sample',
      },
    ],
    points,
  );
  assert.equal(events.length, 2);
  assert.equal(events[0]?.timestamp, 1_700_000_002_000);
  assert.equal(events[0]?.eventType, 'stall');
  assert.equal(events[1]?.timestamp, 1_700_000_025_000);
});

test('grouped-session event markers stay on their source segment', () => {
  const points = [
    { timestamp: 1_000_000, value: 1, segment: 'first' },
    { timestamp: 1_010_000, value: 2, segment: 'first' },
    { timestamp: 1_200_000, value: 3, segment: 'second' },
    { timestamp: 1_210_000, value: 4, segment: 'second' },
  ];
  const events = eventsForChart(
    [
      { session_uuid: 'first', timestamp_unix: 1_005, event_type: 'stall', payload: '' },
      { session_uuid: 'second', timestamp_unix: 1_205, event_type: 'recovery', payload: '' },
      { session_uuid: 'missing', timestamp_unix: 1_205, event_type: 'leak', payload: '' },
    ],
    points,
  );
  assert.deepEqual(
    events.map((event) => [event.sessionId, event.timestamp]),
    [
      ['first', 1_005_000],
      ['second', 1_205_000],
    ],
  );
});

test('time-domain zoom and pan clamp to the full irregular history bounds', () => {
  const full = { start: 1000, end: 10_000 };
  const zoomed = zoomChartRange(full, full, 8_000, 2);
  assert.equal(zoomed.end - zoomed.start, 4_500);
  assert.deepEqual(zoomed, { start: 4500, end: 9000 });
  assert.deepEqual(panChartRange(full, zoomed, 100_000), { start: 5500, end: 10_000 });
  assert.deepEqual(panChartRange(full, zoomed, -100_000), { start: 1000, end: 5500 });
  assert.deepEqual(clampChartRange(full, { start: -5, end: 20_000 }), full);
  assert.deepEqual(zoomChartRange(full, zoomed, 5_500, 0.25), full);
});

test('downsampling retains null telemetry boundaries and missing peaks stay unavailable', () => {
  const points = Array.from({ length: 8 }, (_, index) => ({
    timestamp: index,
    cpu_percent: 20,
    gpu_percent: 30,
    gpu_encoder_percent: 40,
    ram_percent: index === 4 ? null : 60,
    vram_percent: index === 4 ? null : 70,
  }));
  const reduced = downsampleHostHistory(points, 4);
  assert.ok(reduced.some((point) => point.ram_percent == null));
  assert.deepEqual(hostHistoryPeaks([{ timestamp: 1, ram_percent: null, vram_percent: null }]), {
    cpu: null,
    gpu: null,
    encoder: null,
    networkMbps: null,
  });
});
