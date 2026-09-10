import assert from 'node:assert/strict';
import test from 'node:test';
import { hostReadiness, linuxCaptureState } from '../utils/hostReadiness.ts';
import { acknowledgeSettings, configBoolean, settingError } from '../utils/settings.ts';
import { settingsFields } from '../configs/settingsSchema.ts';
import {
  NETWORK_PORT_MAX,
  NETWORK_PORT_MIN,
  networkPortError,
  networkPortRows,
  parseNetworkPort,
} from '../utils/network.ts';

test('capture verification requires observed event-driven KMS, never configuration alone', () => {
  const configured = {
    virtual_display: { ready: true },
    capture_status: { configured_backend: 'kms' },
  };
  assert.equal(linuxCaptureState(configured), 'configured');
  assert.equal(
    linuxCaptureState({
      ...configured,
      capture_status: { ...configured.capture_status, managed_event_driven: true },
    }),
    'configured',
  );
  assert.equal(
    linuxCaptureState({
      ...configured,
      capture_status: { observed_backend: 'kms', managed_event_driven: true },
    }),
    'active',
  );
  assert.equal(linuxCaptureState({}, 'disabled'), 'physical');
  assert.equal(
    linuxCaptureState({ capture_status: { virtual_display_configured: false } }),
    'physical',
  );
  assert.equal(linuxCaptureState({ virtual_display: { ready: false } }), 'unavailable');
  assert.equal(linuxCaptureState({}), 'unknown');
});

test('readiness remains unknown until encoder readiness is known; physical capture does not require virtual outputs', () => {
  assert.equal(hostReadiness(null, false, false), 'unknown');
  assert.equal(hostReadiness({ platform: 'linux' }, false, false), 'unknown');
  const physical = {
    platform: 'linux',
    encoder_status: { state: 'ready' as const },
    virtual_display: { ready: false },
    capture_status: { virtual_display_configured: false },
  };
  assert.equal(hostReadiness(physical, false, false), 'healthy');
  assert.equal(
    hostReadiness(
      { ...physical, capture_status: { virtual_display_configured: true } },
      false,
      false,
    ),
    'warning',
  );
  assert.equal(hostReadiness(physical, true, false), 'streaming');
  assert.equal(hostReadiness(physical, true, true), 'warning');
});

test('acknowledging a save does not swallow edits made after submission', () => {
  const original = { stream_audio: true, mouse: true };
  const submitted = { stream_audio: false };
  const current = { stream_audio: false, mouse: false };
  const baseline = acknowledgeSettings(original, submitted);
  assert.deepEqual(baseline, { stream_audio: false, mouse: true });
  assert.notDeepEqual(baseline, current);
});

test('typed validation catches malformed dimensions, arrays and out-of-range values', () => {
  assert.ok(settingError(settingsFields.get('dd_manual_resolution'), '0x1440'));
  assert.equal(settingError(settingsFields.get('dd_manual_resolution'), '2560x1440'), undefined);
  assert.ok(settingError(settingsFields.get('port'), 999999));
  assert.ok(settingError(settingsFields.get('keybindings'), '{}'));
  assert.equal(settingError(settingsFields.get('keybindings'), '[16, 17]'), undefined);
});

test('network base port bounds protect every derived listener', () => {
  assert.equal(NETWORK_PORT_MIN, 1029);
  assert.equal(NETWORK_PORT_MAX, 65514);
  assert.equal(networkPortError(1028), 'dependent-low');
  assert.equal(networkPortError(1029), undefined);
  assert.deepEqual(
    networkPortRows(1029).map((row) => row.ports),
    ['1024', '1029', '1030', '1050', '1038 - 1040'],
  );
  assert.equal(networkPortError(65514), undefined);
  assert.deepEqual(
    networkPortRows(65514).map((row) => row.ports),
    ['65509', '65514', '65515', '65535', '65523 - 65525'],
  );
  assert.equal(networkPortError(65515), 'dependent-high');
});

test('invalid, empty, decimal and NaN network drafts show no fabricated listeners', () => {
  for (const value of ['', '1029.5', NaN, null, undefined, 'not-a-port']) {
    assert.equal(parseNetworkPort(value), null);
    assert.deepEqual(networkPortRows(value), []);
  }
  assert.equal(
    settingError(settingsFields.get('port'), ''),
    'ui.settings.validation.port_required',
  );
  assert.equal(
    settingError(settingsFields.get('port'), 1029.5),
    'ui.settings.validation.port_integer',
  );
  assert.equal(
    settingError(settingsFields.get('port'), NaN),
    'ui.settings.validation.port_integer',
  );
});

test('API boolean strings keep disabled preferences disabled', () => {
  for (const value of ['false', 'disabled', '0', false])
    assert.equal(configBoolean(value, true), false);
  for (const value of ['true', 'enabled', '1', true]) assert.equal(configBoolean(value), true);
});

test('every runtime override has an editor or belongs to the adapter pair', async () => {
  const { clientOverrideableKeys } = await import('../configs/settingsSchema.ts');
  for (const key of clientOverrideableKeys)
    assert.ok(key === 'adapter_pnp_id' || settingsFields.has(key), key);
});

test('legacy global configuration is covered by v2 fields or a dedicated integration destination', async () => {
  const { readFileSync } = await import('node:fs');
  const { settingsDestinations } = await import('../configs/settingsSchema.ts');
  const source = readFileSync(
    new URL('../../web-legacy/stores/config.ts', import.meta.url),
    'utf8',
  ).split('] as const satisfies')[0];
  for (const match of source.matchAll(/^      (\w+):/gm)) {
    const key = match[1];
    assert.ok(
      key === 'adapter_pnp_id' ||
        settingsFields.has(key) ||
        settingsDestinations.some((destination) => destination.keys.includes(key)),
      key,
    );
  }
});
