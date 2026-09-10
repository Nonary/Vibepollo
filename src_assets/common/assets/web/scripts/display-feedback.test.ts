import assert from 'node:assert/strict';
import test from 'node:test';

import {
  applyDummyPlugVsyncChange,
  dummyPlugVsyncState,
  isWindowsHost,
  windowsDisplayHealth,
} from '../utils/displayHealth.ts';

test('Windows driver health accepts the runtime status contract without false readiness', () => {
  const statuses = [
    [0, 'ready'],
    ['ready', 'ready'],
    [-1, 'failed'],
    [-2, 'version_incompatible'],
    [-3, 'watchdog_failed'],
    ['0', 'ready'],
    ['-2', 'version_incompatible'],
    [1, 'unknown'],
    ['not-a-status', 'unknown'],
  ] as const;

  for (const [status, expected] of statuses) {
    const health = windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: { active: 'vibeshine', configured: 'vibeshine', status },
    });
    assert.equal(health.state, expected);
    assert.equal(health.activeDriver, 'vibeshine');
  }

  const missing = windowsDisplayHealth({ platform: 'windows' });
  assert.equal(missing.state, 'unknown');
  assert.equal(missing.activeDriver, undefined);
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display: { ready: true },
    }).state,
    'unknown',
  );
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: { configured: 'sudovda', status: 1, active: null },
    }).state,
    'uninitialized',
  );
  assert.equal(
    windowsDisplayHealth({
      platform: 'windows',
      virtual_display_driver: { configured: 'vibeshine', status_code: -3, active: 'vibeshine' },
    }).state,
    'watchdog_failed',
  );
});

test('driver health identifies the observed driver independently of a draft selection', () => {
  const health = windowsDisplayHealth({
    platform: 'windows',
    virtual_display_driver: {
      active: 'sudovda',
      configured: 'vibeshine',
      status: 'ready',
    },
  });
  assert.equal(health.activeDriver, 'sudovda');
  assert.equal(health.configuredDriver, 'vibeshine');
  assert.equal(health.state, 'ready');
});

test('driver status is Windows-only and Linux metadata remains absent', () => {
  assert.equal(isWindowsHost({ platform: 'windows' }), true);
  assert.equal(isWindowsHost({ platform: 'linux' }), false);
  assert.equal(isWindowsHost({ platform: 'linux', virtual_display_driver: { status: 0 } }), false);
});

test('dummy-plug HDR forces VSYNC off while enabled and preserves it after disabling', () => {
  assert.deepEqual(dummyPlugVsyncState('true', 'false'), {
    active: true,
    forced: true,
    locked: true,
    effectiveVsyncDisabled: true,
  });
  assert.deepEqual(dummyPlugVsyncState(false, 'true'), {
    active: false,
    forced: false,
    locked: false,
    effectiveVsyncDisabled: true,
  });

  const enabled = applyDummyPlugVsyncChange(
    { dd_wa_dummy_plug_hdr10: false, frame_limiter_disable_vsync: false },
    'on',
  );
  assert.deepEqual(enabled, {
    dd_wa_dummy_plug_hdr10: true,
    frame_limiter_disable_vsync: true,
  });
  assert.deepEqual(applyDummyPlugVsyncChange(enabled, false), {
    dd_wa_dummy_plug_hdr10: false,
    frame_limiter_disable_vsync: true,
  });
});

test('inconsistent persisted dummy-plug config stays clean while showing its effective state', () => {
  const loaded = {
    dd_wa_dummy_plug_hdr10: 'enabled',
    frame_limiter_disable_vsync: '0',
    unrelated_setting: 'preserve',
  };
  const effective = dummyPlugVsyncState(
    loaded.dd_wa_dummy_plug_hdr10,
    loaded.frame_limiter_disable_vsync,
  );
  assert.equal(effective.active, true);
  assert.equal(effective.forced, true);
  assert.equal(effective.effectiveVsyncDisabled, true);
  assert.equal(loaded.frame_limiter_disable_vsync, '0');
  assert.equal(loaded.unrelated_setting, 'preserve');
});

test('disabling dummy-plug HDR does not erase an explicit VSYNC override', () => {
  const draft = { dd_wa_dummy_plug_hdr10: false, frame_limiter_disable_vsync: false };
  const enabled = applyDummyPlugVsyncChange(draft, true);
  const disabled = applyDummyPlugVsyncChange(enabled, false);
  assert.equal(disabled.frame_limiter_disable_vsync, true);
});
