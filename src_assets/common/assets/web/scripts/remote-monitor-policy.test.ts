import assert from 'node:assert/strict';
import test from 'node:test';

import {
  isRemoteMonitorApplication,
  preserveRemoteMonitorRouting,
} from '../../web-legacy/utils/remoteMonitorPolicy.ts';

const monitorUuid = '9a1c5a25-58fe-40e0-b9aa-7d3f00000005';

test('monitor policy follows reserved identity or server metadata, never a display name', () => {
  assert.equal(isRemoteMonitorApplication({ uuid: monitorUuid }), true);
  assert.equal(isRemoteMonitorApplication({ 'remote-session': 'monitor' }), true);
  const ordinary = { uuid: 'ordinary-app', name: 'Remote Monitor' };
  assert.equal(isRemoteMonitorApplication(ordinary), false);
  assert.equal(isRemoteMonitorApplication({ 'remote-session': 'input' }), false);
  assert.equal(isRemoteMonitorApplication(null), false);
  assert.equal(isRemoteMonitorApplication(undefined), false);
});

test('unrelated monitor edits preserve reported routing fields instead of editor defaults', () => {
  const source = {
    uuid: monitorUuid,
    'virtual-screen': true,
    'virtual-display-mode': 'shared',
    'virtual-display-layout': 'extended',
    'dd-configuration-option': 'verify_only',
    'display-output': 'saved-physical-device-id',
  };
  const original = structuredClone(source);
  const payload: Record<string, unknown> = {
    uuid: monitorUuid,
    'virtual-screen': false,
    'virtual-display-mode': 'per_client',
    'virtual-display-layout': 'exclusive',
    'dd-configuration-option': 'ensure_only_display',
    'display-output': '',
    'prefer-10bit-sdr': true,
  };

  preserveRemoteMonitorRouting(source, payload);

  assert.deepEqual(payload, { ...source, 'prefer-10bit-sdr': true });
  assert.deepEqual(source, original);
});

test('hidden monitor controls preserve field absence and explicit false, null, or custom values', () => {
  const source = {
    'remote-session': 'monitor',
    'virtual-screen': false,
    'virtual-display-mode': null,
    'virtual-display-layout': 'future-layout',
  };
  const payload: Record<string, unknown> = {
    'virtual-screen': true,
    'virtual-display-mode': 'disabled',
    'virtual-display-layout': 'exclusive',
    'dd-configuration-option': 'ensure_only_display',
    'display-output': 'editor-default',
  };

  preserveRemoteMonitorRouting(source, payload);

  assert.deepEqual(payload, {
    'virtual-screen': false,
    'virtual-display-mode': null,
    'virtual-display-layout': 'future-layout',
  });
});

test('routing preservation leaves runtime overrides and command-log output persistence alone', () => {
  const source = {
    uuid: monitorUuid,
    output: 'old-command.log',
    'config-overrides': { output_name: 'old-output', dd_configuration_option: 'verify_only' },
  };
  const payload = {
    uuid: monitorUuid,
    output: 'old-command.log',
    'config-overrides': {
      output_name: 'edited-output',
      dd_configuration_option: 'ensure_active',
      remote_monitor_mute_audio: true,
    },
  };
  const expected = structuredClone(payload);

  preserveRemoteMonitorRouting(source, payload);

  assert.deepEqual(payload, expected);
});

test('ordinary shared and physical app routing remains editable', () => {
  for (const routing of [
    { 'virtual-display-mode': 'shared', 'virtual-screen': true },
    { 'virtual-display-mode': 'disabled', 'display-output': 'physical-device-id' },
  ]) {
    const payload = { uuid: 'ordinary-app', ...routing };
    const expected = { ...payload };
    preserveRemoteMonitorRouting({ uuid: 'ordinary-app' }, payload);
    assert.deepEqual(payload, expected);
  }
});

test('new app and Remote Input payloads do not acquire monitor routing policy', () => {
  for (const source of [undefined, null, { 'remote-session': 'input' }]) {
    const payload = { 'virtual-display-mode': 'shared', 'virtual-screen': true };
    const expected = { ...payload };
    preserveRemoteMonitorRouting(source, payload);
    assert.deepEqual(payload, expected);
  }
});

test('explicit duplicate profile policy preserves marker and hidden routing', () => {
  for (const uuid of ['profile-a', 'profile-b']) {
    const source = {
      uuid,
      'remote-monitor-profile': true,
      'virtual-screen': false,
      'display-output': 'ineffective-stored-target',
    };
    const payload: Record<string, unknown> = {
      uuid,
      name: 'Renamed profile',
      'virtual-screen': true,
      'config-overrides': { remote_monitor_mute_audio: false },
    };
    assert.equal(isRemoteMonitorApplication(source), true);
    preserveRemoteMonitorRouting(source, payload);
    assert.equal(payload['remote-monitor-profile'], true);
    assert.equal(payload['virtual-screen'], false);
    assert.equal(payload['display-output'], 'ineffective-stored-target');
    assert.equal(payload.name, 'Renamed profile');
    assert.deepEqual(payload['config-overrides'], { remote_monitor_mute_audio: false });
  }
  assert.equal(isRemoteMonitorApplication({ 'remote-monitor-profile': false }), false);
  assert.equal(isRemoteMonitorApplication({ 'remote-monitor-profile': 'true' }), false);
});
