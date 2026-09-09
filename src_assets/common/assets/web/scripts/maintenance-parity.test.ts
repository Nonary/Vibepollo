import assert from 'node:assert/strict';
import test from 'node:test';

import {
  crashBundlePartPath,
  parseContentDispositionFilename,
  parseCrashBundleManifest,
} from '../utils/maintenanceCrashBundle.ts';

test('crash manifest preserves backend part indexes, names, and ordering', () => {
  const manifest = parseCrashBundleManifest({
    parts: [
      { index: 2, filename: 'bundle-part2.zip', estimated_size_bytes: 20 },
      { index: 1, filename: 'bundle-part1.zip', estimated_size_bytes: 10 },
    ],
  });
  assert.deepEqual(manifest?.parts, [
    { index: 1, filename: 'bundle-part1.zip', estimatedSizeBytes: 10 },
    { index: 2, filename: 'bundle-part2.zip', estimatedSizeBytes: 20 },
  ]);
  assert.equal(crashBundlePartPath(2), '/api/logs/export_crash?part=2');
});

test('invalid or duplicate crash parts are rejected before any download', () => {
  assert.equal(parseCrashBundleManifest({ parts: [] }), null);
  assert.equal(parseCrashBundleManifest({ parts: [{ index: 1 }, { index: 1 }] }), null);
  assert.equal(parseCrashBundleManifest({ parts: [{ index: 0 }] }), null);
  assert.equal(parseCrashBundleManifest({ parts: [{ index: 'part-1' }] }), null);
});

test('content disposition supports UTF-8 and quoted filenames', () => {
  assert.equal(
    parseContentDispositionFilename("attachment; filename*=UTF-8''crash%20part.zip"),
    'crash part.zip',
  );
  assert.equal(parseContentDispositionFilename('attachment; filename="crash.zip"'), 'crash.zip');
  assert.equal(parseContentDispositionFilename(null), null);
});
