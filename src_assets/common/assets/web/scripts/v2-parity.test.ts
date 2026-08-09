import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import {
  displayFieldVisibility,
  downsampleHostHistory,
  hostHistoryPeaks,
  normalizeCommandRows,
  preserveHiddenDisplayValues,
  serializeCommandRows,
} from '../utils/v2Parity.ts';

test('global command rows preserve order, verbatim text, and Windows elevation', () => {
  const source = [
    { do: '  set-mode "A"  ', undo: 'restore A', elevated: true, custom: 'keep' },
    { do: 'second', undo: '', elevated: false },
  ];
  const rows = normalizeCommandRows(source, 'windows');
  assert.deepEqual(serializeCommandRows(rows, 'windows'), source);
  assert.deepEqual(serializeCommandRows(source, 'linux'), [
    { do: '  set-mode "A"  ', undo: 'restore A', custom: 'keep' },
    { do: 'second', undo: '' },
  ]);
});

test('display visibility calls disabled Physical and does not clear hidden values', () => {
  assert.deepEqual(displayFieldVisibility('disabled'), { physical: true, virtual: false });
  assert.deepEqual(displayFieldVisibility('per_client'), { physical: false, virtual: true });
  assert.deepEqual(
    preserveHiddenDisplayValues(
      { dd_virtual_display_scale: 125 },
      { virtual_display_mode: 'disabled' },
    ),
    { dd_virtual_display_scale: 125, virtual_display_mode: 'disabled' },
  );
});

test('host history downsampling and peaks make relative spikes comparable', () => {
  const points = Array.from({ length: 10 }, (_, index) => ({
    timestamp: index,
    cpu_percent: index === 7 ? 95 : 10,
    gpu_percent: index === 6 ? 88 : 20,
    gpu_encoder_percent: index === 8 ? 79 : 5,
    net_tx_bps: index === 9 ? 12_000_000 : 1_000_000,
  }));
  const downsampled = downsampleHostHistory(points, 4);
  assert.equal(downsampled.length, 4);
  assert.deepEqual(
    downsampled.map((point) => point.timestamp),
    [6, 7, 8, 9],
    'the rendered series must retain each CPU/GPU/encoder/network spike',
  );
  assert.deepEqual(hostHistoryPeaks(points), { cpu: 95, gpu: 88, encoder: 79, networkMbps: 12 });
});

test('host compute readouts label current and peak values explicitly', () => {
  const chart = readFileSync(
    new URL('../components/stats/HostComputeChart.vue', import.meta.url),
    'utf8',
  );
  assert.match(
    chart,
    /CPU[\s\S]*t\('stats\.current'\)[\s\S]*current\.cpu[\s\S]*t\('stats\.peak'\)[\s\S]*peak\.cpu/,
  );
  assert.match(
    chart,
    /GPU[\s\S]*t\('stats\.current'\)[\s\S]*current\.gpu[\s\S]*t\('stats\.peak'\)[\s\S]*peak\.gpu/,
  );
  assert.match(
    chart,
    /ENC[\s\S]*t\('stats\.current'\)[\s\S]*current\.encoder[\s\S]*t\('stats\.peak'\)[\s\S]*peak\.encoder/,
  );
  assert.doesNotMatch(chart, /t\('stats\.peak'\)[^\n]*\/[^\n]*t\('stats\.current'\)/);
});
