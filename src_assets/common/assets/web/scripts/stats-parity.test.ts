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
