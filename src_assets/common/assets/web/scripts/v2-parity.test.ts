import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import { clientImageWidthPresets } from '../configs/clientImageWidthPresets.ts';
import { settingsCategories } from '../configs/settingsSchema.ts';
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

test('client image-width suggestions reach the field and stay inside what the host accepts', () => {
  const field = settingsCategories
    .flatMap((category) => category.groups)
    .flatMap((group) => group.fields)
    .find((candidate) => candidate.key === 'dd_virtual_display_image_width_mm');
  assert.ok(field, 'the client image width setting must exist to hang suggestions off');

  assert.deepEqual(
    field.presets,
    clientImageWidthPresets.map((preset) => ({
      label: `${preset.label} - ${preset.mode}`,
      value: String(preset.widthMm),
    })),
    'every row of the preset table has to reach the field, in table order',
  );

  // config::apply_config() drops a width outside 10-2000 mm with a warning, so a suggestion
  // outside that band would be offered and then silently ignored by the host.
  for (const preset of clientImageWidthPresets) {
    assert.ok(
      Number.isInteger(preset.widthMm) && preset.widthMm >= 10 && preset.widthMm <= 2000,
      `${preset.label} must be a whole number of millimetres the host will accept`,
    );
    assert.ok(preset.label && preset.mode, `${preset.label} must name a device and a mode`);
  }

  const values = clientImageWidthPresets.map((preset) => preset.widthMm);
  assert.equal(new Set(values).size, values.length, 'two devices must not share one width');
  const labels = clientImageWidthPresets.map((preset) => `${preset.label} - ${preset.mode}`);
  assert.equal(new Set(labels).size, labels.length, 'suggestion labels must be distinguishable');
});

test('the worked examples keep the widths the docs and the C++ tests are written against', () => {
  // tests/unit/platform/windows/test_virtual_display_sunshine.cpp asserts these two widths
  // resolve to 200% and 450%; docs/configuration.md quotes the same measurements. Changing a
  // width here without changing those leaves three descriptions of one number disagreeing.
  const widthFor = (label: string): number | undefined =>
    clientImageWidthPresets.find((preset) => preset.label === label)?.widthMm;
  assert.equal(widthFor('Alienware m16 R2'), 345);
  assert.equal(widthFor('Galaxy Z Fold8 Ultra (inner panel)'), 151);
});
