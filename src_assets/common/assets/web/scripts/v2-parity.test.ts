import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

import {
  captureOptionsForPlatform,
  frameGenerationOptionsForPlatform,
  gamepadOptionsForPlatform,
  settingsDefaults,
  settingsCategories,
  type SettingsField,
} from '../configs/settingsSchema.ts';
import {
  displayFieldVisibility,
  downsampleHostHistory,
  hostHistoryPeaks,
  normalizeCommandRows,
  preserveHiddenDisplayValues,
  serializeCommandRows,
} from '../utils/v2Parity.ts';

test('capture options follow the host platform', () => {
  assert.deepEqual(
    captureOptionsForPlatform('linux').map((option) => option.value),
    ['', 'kms', 'kwin', 'portal', 'wlr', 'x11', 'nvfbc'],
  );
  assert.deepEqual(
    captureOptionsForPlatform('windows').map((option) => option.value),
    ['', 'wgc', 'wgcc', 'ddx'],
  );
  assert.deepEqual(
    captureOptionsForPlatform('macos').map((option) => option.value),
    [''],
  );
});

test('Linux remote-monitor controls and frame-generation labels are available', () => {
  for (const key of [
    'remote_monitor_mute_audio',
    'remote_monitor_disconnect_on_stream_end',
    'remote_monitor_disconnect_on_client_disconnect',
    'remote_monitor_terminate_on_first_request',
  ]) {
    assert.equal(matchesPlatform(settingsFields.get(key)!, 'linux'), true);
    assert.equal(matchesPlatform(settingsFields.get(key)!, 'windows'), true);
    assert.equal(matchesPlatform(settingsFields.get(key)!, 'macos'), false);
  }
  const messages = JSON.parse(readFileSync(
    new URL('../public/assets/locale/ui/en.json', import.meta.url), 'utf8',
  ));
  for (const option of frameGenerationOptionsForPlatform('linux')) {
    const label = option.labelKey.split('.').reduce((value, key) => value?.[key], messages);
    assert.equal(typeof label, 'string', option.labelKey);
    assert.ok(label.length);
  }
});

test('gamepad options follow the host platform', () => {
  assert.deepEqual(
    gamepadOptionsForPlatform('linux').map((option) => option.value),
    ['auto', 'xone', 'ds4', 'ds5', 'switch'],
  );
  assert.deepEqual(
    gamepadOptionsForPlatform('windows').map((option) => option.value),
    ['auto', 'x360', 'ds4', 'vhf', 'vhf_xbox', 'vhf_xbox_one', 'vhf_ds4', 'vhf_ds5', 'vhf_switch'],
  );
  const legacyOptions = readFileSync(
    new URL('../../web-legacy/configs/configSelectOptions.ts', import.meta.url),
    'utf8',
  );
  assert.match(legacyOptions, /linux:\s*\['xone', 'ds4', 'ds5', 'switch'\]/);
});

test('Linux keeps common virtual-display policy and hides Windows display internals', () => {
  const fields = settingsCategories.flatMap((category) =>
    category.groups.flatMap((group) => group.fields),
  );
  const commonKeys = new Set([
    'virtual_display_mode',
    'virtual_display_layout',
    'dd_resolution_option',
    'dd_refresh_rate_option',
    'dd_hdr_option',
    'dd_hdr_request_override',
  ]);
  const windowsOnlyKeys = new Set([
    'dd_use_sunshine_virtual_display_driver',
    'dd_activate_virtual_display',
    'dd_virtual_display_permanent_count',
    'dd_display_helper_engine',
    'vulkan_hdr_layer',
    'dd_wa_dummy_plug_hdr10',
    'dd_always_restore_from_golden',
    'dd_snapshot_restore_hotkey',
    'dd_snapshot_restore_hotkey_modifiers',
    'wgc_pacing_smoothing',
    'always_send_scancodes',
    'native_pen_touch',
    'install_steam_audio_drivers',
  ]);

  for (const field of fields.filter((candidate) => commonKeys.has(candidate.key))) {
    assert.equal(field.platform, undefined, `${field.key} must remain cross-platform`);
  }
  for (const field of fields.filter((candidate) => windowsOnlyKeys.has(candidate.key))) {
    assert.equal(field.platform, 'windows', `${field.key} must remain Windows-only`);
  }
  for (const key of [
    'gamepad',
    'ds4_back_as_touchpad_click',
    'dd_virtual_display_scale',
    'dd_config_revert_delay',
    'dd_config_revert_on_disconnect',
    'dd_paused_virtual_display_timeout_secs',
  ]) {
    const field = fields.find((candidate) => candidate.key === key);
    assert.deepEqual(field?.platform, ['windows', 'linux'], `${key} must support Linux cleanup`);
  }
});

test('Linux exposes Remote Monitor behavior controls', () => {
  const remoteMonitor = settingsCategories
    .flatMap((category) => category.groups)
    .find((group) => group.id === 'everyday_remote_monitor');
  assert.ok(remoteMonitor);

  for (const key of [
    'remote_monitor_mute_audio',
    'remote_monitor_disconnect_on_stream_end',
    'remote_monitor_disconnect_on_client_disconnect',
    'remote_monitor_terminate_on_first_request',
  ]) {
    const field: SettingsField | undefined = remoteMonitor.fields.find(
      (candidate) => candidate.key === key,
    );
    assert.deepEqual(field?.platform, ['windows', 'linux'], `${key} must support Linux`);
  }
});

test('Linux virtual-display pacing uses Linux-specific copy', () => {
  assert.deepEqual(
    frameGenerationOptionsForPlatform('linux').map((option) => option.labelKey),
    [
      'ui.settings.options.frame_generation.automatic_linux',
      'ui.settings.options.frame_generation.compatibility_linux',
      'ui.settings.options.frame_generation.off_linux',
    ],
  );

  const messages = JSON.parse(
    readFileSync(new URL('../public/assets/locale/ui/en.json', import.meta.url), 'utf8'),
  );
  const linuxCopy = [
    messages.ui.settings.fields.frame_limiter_auto_virtual_framegen.description_linux,
    messages.ui.settings.groups.everyday_remote_monitor.description,
    messages.ui.settings.groups.everyday_smoothness.description_linux,
    messages.ui.settings.options.frame_generation.automatic_linux,
    messages.ui.settings.options.frame_generation.compatibility_linux,
    messages.ui.settings.options.frame_generation.off_linux,
    messages.ui.settings.summary.automatic_pacing_linux,
    messages.ui.settings.summary.automatic_pacing_limiter_off_linux,
    messages.ui.settings.summary.compatibility_pacing_linux,
    messages.ui.settings.summary.compatibility_pacing_limiter_off_linux,
  ];
  for (const copy of linuxCopy) {
    assert.equal(typeof copy, 'string');
    assert.doesNotMatch(copy, /\b(?:WGC|Windows)\b/i);
  }
});

test('Linux Proton and MangoHUD limiter choices stay aligned with legacy UI', () => {
  assert.equal(settingsDefaults.frame_limiter_provider, 'auto');
  assert.equal(settingsDefaults.mangohud_limiter_method, 'late');

  const fields = settingsCategories.flatMap((category) =>
    category.groups.flatMap((group) => group.fields),
  );
  const method = fields.find((field) => field.key === 'mangohud_limiter_method');
  assert.deepEqual(method?.platform, 'linux');
  assert.deepEqual(method?.visibleWhen, {
    key: 'frame_limiter_provider',
    equals: 'mangohud',
  });
  assert.deepEqual(
    method?.options?.map((option) => option.value),
    ['early', 'late'],
  );

  const messages = JSON.parse(
    readFileSync(new URL('../public/assets/locale/ui/en.json', import.meta.url), 'utf8'),
  );
  assert.match(messages.ui.integrations.mangohud.providerAuto, /Proton.*MangoHUD/i);
  assert.match(messages.ui.integrations.mangohud.limiterMethodDescription, /latency/i);
  assert.match(messages.ui.integrations.mangohud.limiterMethodDescription, /frame generation/i);

  const legacyStep = readFileSync(
    new URL('../../web-legacy/configs/tabs/audiovideo/FrameLimiterStep.vue', import.meta.url),
    'utf8',
  );
  assert.match(legacyStep, /value: 'mangohud-proton'/);
  assert.match(legacyStep, /value: 'proton'/);
  assert.match(legacyStep, /setting-key="mangohud_limiter_method"/);
});

test('Linux maintenance omits Windows-only support and recovery sections', () => {
  const maintenanceView = readFileSync(
    new URL('../views/MaintenanceView.vue', import.meta.url),
    'utf8',
  );
  assert.match(maintenanceView, /v-if="isWindows"[\s\S]*aria-labelledby="display-recovery-title"/);
  assert.match(maintenanceView, /v-if="isWindows"[^>]*aria-labelledby="support-title"/);
  assert.doesNotMatch(maintenanceView, /ui\.maintenance\.support\.windowsUnavailable/);
});

test('Linux uses display enumeration and persistence reset', () => {
  const settingsView = readFileSync(new URL('../views/SettingsView.vue', import.meta.url), 'utf8');
  assert.match(settingsView, /isWindowsHost\.value \|\| isLinuxHost\.value/);
  assert.match(settingsView, /v-if="supportsDisplayDeviceEnumeration"/);
  assert.match(
    settingsView,
    /v-if="\(isWindowsHost \|\| isLinuxHost\) && activeCategory === 'display' && !isSearching"/,
  );
});

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

test('persisted command JSON is available to the v2 editor', () => {
  const persisted = JSON.stringify([{ do: 'connect', undo: 'disconnect', elevated: true }]);
  assert.deepEqual(normalizeCommandRows(persisted, 'windows'), [
    { do: 'connect', undo: 'disconnect', elevated: true },
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

test('Settings protects drafts and keeps restart actions available', () => {
  const settingsView = readFileSync(new URL('../views/SettingsView.vue', import.meta.url), 'utf8');
  assert.match(settingsView, /<form[\s\S]*@submit\.prevent="save"/);
  assert.match(settingsView, /class="button button--primary" type="submit"/);
  assert.match(settingsView, /:disabled="loading \|\| saving \|\| isDirty"/);
  assert.match(settingsView, /restartAvailable\.value \|\|= Boolean\(result\.restartRequired\)/);
  assert.match(settingsView, /v-else-if="notice \|\| restartAvailable"/);
  assert.match(settingsView, /:disabled="restarting"/);
});

test('Settings explains unavailable host metadata and virtual-display readiness', () => {
  const settingsView = readFileSync(new URL('../views/SettingsView.vue', import.meta.url), 'utf8');
  assert.match(settingsView, /metadataUnavailable\.value = true/);
  assert.match(settingsView, /virtualDisplayUnavailable/);

  const messages = JSON.parse(
    readFileSync(new URL('../public/assets/locale/ui/en.json', import.meta.url), 'utf8'),
  );
  assert.equal(typeof messages.ui.settings.metadata_unavailable.description, 'string');
  assert.equal(typeof messages.ui.settings.virtual_display_unavailable.description, 'string');
});
