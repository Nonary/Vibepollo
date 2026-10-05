import { test, expect, type Page } from '@playwright/test';

interface HostOptions {
  platform?: 'windows' | 'linux';
  apps?: Array<Record<string, unknown>>;
  browse?: boolean;
  playniteUndetected?: boolean;
  golden?: Record<string, unknown>;
  config?: Record<string, unknown>;
  termination?: Record<string, unknown>;
}

async function setupHost(page: Page, options: HostOptions = {}) {
  const platform = options.platform ?? 'windows';
  const apps = options.apps ?? [];
  let currentApps = [...apps];
  const currentConfig: Record<string, unknown> = {
    capture: 'wgc',
    encoder: 'nvenc',
    lossless_scaling_path: '',
    lossless_scaling_legacy_auto_detect: false,
    playnite_auto_sync: true,
    ...options.config,
  };
  const goldenStatus: Record<string, unknown> = { exists: false, ...options.golden };
  const calls = {
    crashManifest: 0,
    crashParts: [] as number[],
    launch: 0,
    purgeAutosync: 0,
    appDeletes: [] as string[],
    browse: [] as string[],
    configPatches: [] as Record<string, unknown>[],
    playniteStatus: 0,
    failGoldenStatus: false,
    goldenStatusReads: 0,
    goldenExports: 0,
    goldenDeletes: 0,
    displayResets: 0,
    displayTerminations: 0,
  };
  let failedPartTwo = true;

  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    if (!url.pathname.startsWith('/api/')) {
      await route.continue();
      return;
    }
    const path = url.pathname;
    const method = request.method();
    let body: unknown = { status: true };

    if (path === '/api/auth/status') {
      body = { authenticated: true, login_required: false, credentials_configured: true };
    } else if (path === '/api/configLocale') {
      body = { locale: 'en' };
    } else if (path === '/api/csrf-token') {
      body = { csrf_token: 'test-token' };
    } else if (path === '/api/metadata') {
      body = {
        platform,
        version: '1.0.0',
        prerelease: '',
        status: true,
        windows_build_number: 26100,
        encoder_status: { state: 'ready', h264: true },
        capture_status: { virtual_display_configured: true },
      };
    } else if (path === '/api/auth/sessions') {
      body = { sessions: [] };
    } else if (path === '/api/health/crashdump') {
      body = { available: true, filename: 'crash.dmp', size_bytes: 1234 };
    } else if (path === '/api/display/golden_status') {
      calls.goldenStatusReads += 1;
      if (calls.failGoldenStatus) {
        await route.fulfill({ status: 503, json: { error: 'status unavailable' } });
        return;
      }
      body = goldenStatus;
    } else if (path === '/api/display/export_golden' && method === 'POST') {
      calls.goldenExports += 1;
    } else if (path === '/api/display/golden' && method === 'DELETE') {
      calls.goldenDeletes += 1;
      body = { status: true, deleted: true };
    } else if (path === '/api/reset-display-device-persistence' && method === 'POST') {
      calls.displayResets += 1;
    } else if (path === '/api/display/terminate_virtual' && method === 'POST') {
      calls.displayTerminations += 1;
      body = options.termination ?? { status: true, topology_restored: true };
    } else if (path === '/api/apps') {
      body = { apps: currentApps };
    } else if (path === '/api/playnite/status') {
      calls.playniteStatus += 1;
      body = {
        enabled: true,
        available: true,
        active: false,
        installed: true,
        extensions_dir: options.playniteUndetected ? '' : 'C:\\Playnite\\Extensions',
        installed_version: '1.0.0',
        packaged_version: '1.0.0',
        update_available: false,
      };
    } else if (path === '/api/playnite/categories' || path === '/api/playnite/games') {
      body = [];
    } else if (path === '/api/playnite/launch' && method === 'POST') {
      calls.launch += 1;
      body = { status: true };
    } else if (path === '/api/apps/purge_autosync' && method === 'POST') {
      calls.purgeAutosync += 1;
      const removed = currentApps.filter((app) => app['playnite-managed'] === 'auto').length;
      currentApps = currentApps.filter((app) => app['playnite-managed'] !== 'auto');
      body = {
        status: true,
        removed,
      };
    } else if (method === 'DELETE' && /^\/api\/apps\/[^/]+$/.test(path)) {
      const uuid = decodeURIComponent(path.slice('/api/apps/'.length));
      calls.appDeletes.push(uuid);
      currentApps = currentApps.filter((app) => app.uuid !== uuid);
      body = { status: true };
    } else if (path === '/api/steam/status') {
      body = { enabled: true, available: true, game_count: 0 };
    } else if (path === '/api/steam/games') {
      body = [];
    } else if (path === '/api/rtss/status') {
      body = { enabled: false, path_exists: false };
    } else if (path === '/api/lossless_scaling/status') {
      body = options.browse
        ? { status: 'not-configured', candidates: [] }
        : { status: 'detected', resolved_path: 'C:\\LosslessScaling\\LosslessScaling.exe' };
    } else if (path === '/api/vigembus/status') {
      body = { installed: true, version_compatible: true };
    } else if (path === '/api/health/vulkan-hdr-layer') {
      body = { installed: true, enabled: false };
    } else if (path === '/api/config' && method === 'GET') {
      body = currentConfig;
    } else if (path === '/api/config' && method === 'PATCH') {
      const patch = request.postDataJSON() as Record<string, unknown>;
      calls.configPatches.push(patch);
      Object.assign(currentConfig, patch);
      if ('dd_configuration_option' in patch || 'virtual_display_mode' in patch) {
        goldenStatus.maintenance_available =
          String(currentConfig.dd_configuration_option ?? 'disabled') !== 'disabled' ||
          String(currentConfig.virtual_display_mode ?? 'disabled') !== 'disabled';
      }
      body = { status: true };
    } else if (path === '/api/browse') {
      const browsePath = url.searchParams.get('path') ?? '';
      calls.browse.push(browsePath);
      if (!browsePath) {
        body = {
          path: '',
          parent: '',
          entries: [
            { name: 'C:\\', path: 'C:\\', type: 'directory' },
            { name: '\\\\server\\share', path: '\\\\server\\share', type: 'directory' },
          ],
        };
      } else if (browsePath === 'C:\\') {
        body = { path: 'C:\\', parent: 'C:\\', entries: [] };
      } else if (browsePath === '\\\\server\\share') {
        body = {
          path: '\\\\server\\share',
          parent: '\\\\server',
          entries: [{ name: 'Games', path: '\\\\server\\share\\Games', type: 'directory' }],
        };
      } else {
        body = {
          path: '\\\\server\\share\\Games',
          parent: '\\\\server\\share',
          entries: [
            {
              name: 'LosslessScaling.exe',
              path: '\\\\server\\share\\Games\\LosslessScaling.exe',
              type: 'file',
            },
          ],
        };
      }
    } else if (path === '/api/logs/export_crash/manifest') {
      calls.crashManifest += 1;
      body = {
        parts: [
          { index: 1, filename: 'crash-part1.zip', estimated_size_bytes: 10 },
          { index: 2, filename: 'crash-part2.zip', estimated_size_bytes: 20 },
        ],
      };
    } else if (path === '/api/logs/export_crash') {
      const index = Number(url.searchParams.get('part') ?? 1);
      calls.crashParts.push(index);
      if (index === 2 && failedPartTwo) {
        failedPartTwo = false;
        await route.fulfill({ status: 503, json: { error: 'part unavailable' } });
        return;
      }
      await route.fulfill({
        status: 200,
        body: `zip-part-${index}`,
        headers: {
          'content-type': 'application/zip',
          'content-disposition': `attachment; filename="crash-part${index}.zip"`,
        },
      });
      return;
    }

    await route.fulfill({ json: body });
  });
  await page.route('**/assets/changelog.json', async (route) =>
    route.fulfill({ json: { releases: [] } }),
  );
  return calls;
}

test('Windows maintenance exposes every crash part and recovers a failed part', async ({
  page,
}) => {
  const calls = await setupHost(page);
  await page.goto('/v2/maintenance');
  await expect(page.getByRole('button', { name: 'Download crash bundle' })).toBeVisible();
  await page.getByRole('button', { name: 'Download crash bundle' }).click();
  await expect(page.getByText('Crash bundle parts', { exact: true })).toBeVisible();
  await expect(page.getByText('crash-part1.zip', { exact: true })).toBeVisible();
  await expect(page.getByText('crash-part2.zip', { exact: true })).toBeVisible();
  await expect(page.getByText(/parts 2 failed/)).toBeVisible();
  expect(calls.crashManifest).toBe(1);
  expect(calls.crashParts).toEqual([1, 2]);

  await page.getByRole('button', { name: 'Retry part' }).click();
  await expect.poll(() => calls.crashParts).toEqual([1, 2, 2]);
  await expect(page.getByText(/Crash bundle downloads started/)).toBeVisible();
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-crash-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-crash-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
});

test('Windows Playnite launch is actionable and auto-sync purge is separately confirmed', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    apps: [
      { uuid: 'auto-1', name: 'Managed game', 'playnite-id': 'p1', 'playnite-managed': 'auto' },
      { uuid: 'manual-1', name: 'Manual game', 'playnite-id': 'p2' },
      { uuid: 'steam-1', name: 'Steam game', 'steam-id': 's1', 'steam-managed': 'auto' },
    ],
  });
  await page.goto('/v2/integrations');
  await expect(page.getByRole('button', { name: 'Launch Playnite' })).toBeVisible();
  await page.getByRole('button', { name: 'Launch Playnite' }).click();
  await expect.poll(() => calls.launch).toBe(1);

  await expect(page.getByRole('button', { name: 'Remove auto-synced apps' })).toBeVisible();
  await page.getByRole('button', { name: 'Remove auto-synced apps' }).click();
  await expect(page.getByRole('dialog')).toContainText('currently 1');
  expect(calls.purgeAutosync).toBe(0);
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-playnite-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-playnite-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  expect(calls.purgeAutosync).toBe(0);

  await page.getByRole('button', { name: 'Remove auto-synced apps' }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Remove auto-synced apps' }).click();
  await expect.poll(() => calls.purgeAutosync).toBe(1);
  await expect(page.getByRole('button', { name: 'Remove auto-synced apps' })).toHaveCount(0);
});

test('Lossless picker browses host roots and UNC directories without saving until selected', async ({
  page,
}) => {
  const calls = await setupHost(page, { browse: true });
  await page.goto('/v2/settings?category=pacing');
  await expect(page.locator('#setting-lossless_scaling_path')).toBeVisible();
  await page.getByRole('button', { name: 'Browse host' }).click();
  const picker = page.getByRole('dialog');
  await expect(picker).toBeVisible();
  await picker.getByRole('option', { name: /server\\share/ }).click();
  await picker.getByRole('option', { name: /Games/ }).click();
  await picker.getByRole('option', { name: /LosslessScaling\.exe/ }).click();
  expect(calls.configPatches).toEqual([]);
  await expect(
    picker.getByText('Selected executable: \\\\server\\share\\Games\\LosslessScaling.exe'),
  ).toBeVisible();
  await page.getByRole('dialog').getByRole('button', { name: 'Use selected path' }).click();
  await expect(page.locator('#setting-lossless_scaling_path')).toHaveValue(
    '\\\\server\\share\\Games\\LosslessScaling.exe',
  );
  expect(calls.configPatches).toEqual([]);
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => calls.configPatches.length).toBe(1);
  expect(calls.configPatches[0]).toEqual({
    lossless_scaling_path: '\\\\server\\share\\Games\\LosslessScaling.exe',
  });
  await page.getByRole('button', { name: 'Browse host' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  await expect(
    page.getByRole('dialog').getByRole('option', { name: /LosslessScaling\.exe/ }),
  ).toBeVisible();
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-picker-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/maintenance-picker-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
  expect(calls.browse[0]).toBe('');
  expect(calls.browse).toContain('\\\\server\\share');
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toHaveCount(0);
});

test('Windows library purge keeps non-Playnite applications', async ({ page }) => {
  const calls = await setupHost(page, {
    apps: [
      {
        uuid: 'managed-playnite',
        name: 'Managed game',
        'playnite-id': 'p1',
        'playnite-managed': 'auto',
      },
      { uuid: 'manual-playnite', name: 'Manual game', 'playnite-id': 'p2' },
      { uuid: 'fullscreen-playnite', name: 'Playnite (Fullscreen)', cmd: ['--fullscreen'] },
      { uuid: 'steam-game', name: 'Steam game', 'steam-id': 's1' },
      { uuid: 'custom-app', name: 'Custom app', cmd: ['custom.exe'] },
    ],
  });
  await page.goto('/v2/library');
  await expect(page.locator('.library-page')).toBeVisible();
  const purgeButton = page.getByRole('button', { name: 'Remove Playnite entries' });
  await expect(purgeButton).toBeVisible();
  await purgeButton.click();
  await expect(page.getByRole('dialog')).toContainText('currently 3');
  expect(calls.appDeletes).toEqual([]);
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel' }).click();
  expect(calls.appDeletes).toEqual([]);

  await purgeButton.click();
  await page.getByRole('dialog').getByRole('button', { name: 'Remove Playnite entries' }).click();
  await expect
    .poll(() => calls.appDeletes)
    .toEqual(['managed-playnite', 'manual-playnite', 'fullscreen-playnite']);
  await expect(purgeButton).toHaveCount(0);
});

test('Linux integrations do not query or expose Playnite', async ({ page }) => {
  const calls = await setupHost(page, { platform: 'linux' });
  await page.goto('/v2/integrations');
  await expect(page.locator('.integrations-page')).toBeVisible();
  await expect(page.getByText('Playnite', { exact: true })).toHaveCount(0);
  expect(calls.playniteStatus).toBe(0);
});

test('Linux virtual display termination requires confirmation and avoids Windows recovery APIs', async ({
  page,
}) => {
  const calls = await setupHost(page, { platform: 'linux' });
  await page.goto('/v2/maintenance');
  await expect(
    page.getByRole('heading', { name: 'Virtual display recovery', exact: true }),
  ).toBeVisible();
  await expect(page.getByRole('button', { name: 'Capture snapshot', exact: true })).toHaveCount(0);
  const terminate = page.getByRole('button', { name: 'Terminate virtual display', exact: true });
  await terminate.click();
  const dialog = page.getByRole('dialog');
  await expect(dialog).toContainText('Active streams using a virtual display will lose video.');
  await expect(dialog).toContainText('A host without a working physical display may go blank.');
  expect(calls.displayTerminations).toBe(0);
  await dialog.getByRole('button', { name: 'Cancel', exact: true }).click();
  expect(calls.displayTerminations).toBe(0);
  await terminate.click();
  await dialog.getByRole('button', { name: 'Terminate virtual display', exact: true }).click();
  await expect(
    page.getByText('Managed virtual displays were disconnected.', { exact: true }),
  ).toBeVisible();
  expect(calls.displayTerminations).toBe(1);
  expect(calls.goldenStatusReads).toBe(0);
  expect(calls.goldenExports).toBe(0);
  expect(calls.displayResets).toBe(0);
  expect(calls.configPatches).toEqual([]);
});

for (const status of [false, undefined]) {
  test(`Linux termination reports ${status === false ? 'failed' : 'unconfirmed'} removal as an error`, async ({
    page,
  }) => {
    const calls = await setupHost(page, {
      platform: 'linux',
      termination: status === false ? { status, error: 'Connector removal failed' } : {},
    });
    await page.goto('/v2/maintenance');
    await page.getByRole('button', { name: 'Terminate virtual display', exact: true }).click();
    await page
      .getByRole('dialog')
      .getByRole('button', { name: 'Terminate virtual display', exact: true })
      .click();
    await expect(
      page
        .getByText(
          status === false
            ? 'Connector removal failed'
            : 'One or more managed virtual displays could not be terminated.',
          { exact: true },
        )
        .first(),
    ).toBeVisible();
    await expect(
      page.getByText('Managed virtual displays were disconnected.', { exact: true }),
    ).toHaveCount(0);
    expect(calls.displayTerminations).toBe(1);
  });
}

test('Linux termination distinguishes successful removal from failed physical restoration', async ({
  page,
}) => {
  await setupHost(page, {
    platform: 'linux',
    termination: { status: true, topology_restored: false },
  });
  await page.goto('/v2/maintenance');
  await page.getByRole('button', { name: 'Terminate virtual display', exact: true }).click();
  await page
    .getByRole('dialog')
    .getByRole('button', { name: 'Terminate virtual display', exact: true })
    .click();
  await expect(
    page.getByText(
      'Managed virtual displays were disconnected, but the physical display layout could not be restored.',
      { exact: true },
    ),
  ).toBeVisible();
});

test('Playnite detection failure exposes a host directory picker and saves its selection', async ({
  page,
}) => {
  const calls = await setupHost(page, { playniteUndetected: true });
  await page.goto('/v2/integrations');
  const policies = page.locator('#playnite-policies');
  await expect(policies).toHaveAttribute('open', '');
  await policies.getByRole('button', { name: 'Browse host' }).click();
  const dialog = page.getByRole('dialog');
  await expect(dialog).toHaveAccessibleName('Choose Playnite directory');
  await dialog.getByRole('option', { name: /C:/ }).click();
  await expect(dialog).toContainText('Selected folder: C:\\');
  await dialog.getByRole('button', { name: 'Use selected path' }).click();
  await expect(policies.getByLabel('Playnite directory', { exact: true })).toHaveValue('C:\\');
  expect(calls.configPatches).toEqual([]);
  await policies.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => calls.configPatches).toEqual([{ playnite_install_dir: 'C:\\' }]);
});
test('disabled display maintenance exposes session recovery without activating the helper', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    golden: {
      maintenance_available: false,
      session_current_exists: true,
      session_previous_exists: false,
      restore_task_state: 'disabled',
      helper_engine: 'legacy',
    },
  });
  await page.goto('/v2/maintenance');
  await expect(
    page.getByText(
      'Session recovery: current snapshot present, previous snapshot absent. Restore task: disabled by the user. Helper engine: v1.',
    ),
  ).toBeVisible();
  await expect(
    page.getByText(/Display maintenance requires display automation or a virtual display/),
  ).toBeVisible();
  await expect(page.getByRole('button', { name: 'Capture snapshot', exact: true })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Capture snapshot', exact: true })).toBeDisabled();
  await expect(
    page.getByRole('button', { name: 'Terminate virtual display', exact: true }),
  ).toBeEnabled();
  expect(calls.goldenExports).toBe(0);
  expect(calls.goldenDeletes).toBe(0);
  expect(calls.configPatches).toEqual([]);
});

test('existing snapshot maintenance stays visible and disabled when automation is off', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    golden: {
      exists: true,
      maintenance_available: false,
      restore_task_state: 'enabled',
      helper_engine: 'v2',
    },
  });
  await page.goto('/v2/maintenance');
  await expect(page.getByRole('button', { name: 'Replace snapshot', exact: true })).toBeDisabled();
  await expect(page.getByRole('button', { name: 'Delete snapshot', exact: true })).toBeDisabled();
  await expect(page.getByText(/Restore task: enabled. Helper engine: v2./)).toBeVisible();
  expect(calls.goldenExports).toBe(0);
  expect(calls.goldenDeletes).toBe(0);
});

test('available display maintenance preserves confirmation and normal capture', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    golden: { maintenance_available: true, restore_task_state: 'enabled', helper_engine: 'v2' },
  });
  await page.goto('/v2/maintenance');
  await page.getByRole('button', { name: 'Capture snapshot', exact: true }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  expect(calls.goldenExports).toBe(0);
  await page
    .getByRole('dialog')
    .getByRole('button', { name: 'Capture snapshot', exact: true })
    .click();
  await expect.poll(() => calls.goldenExports).toBe(1);
  expect(calls.configPatches).toEqual([]);
});

test('display settings explain disabled capture and reset without changing saved automation', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    golden: { maintenance_available: false },
    config: { dd_configuration_option: 'disabled', virtual_display_mode: 'disabled' },
  });
  await page.goto('/v2/settings?category=display');
  await expect(page.getByRole('button', { name: 'Create snapshot', exact: true })).toBeDisabled();
  await expect(
    page.getByRole('button', { name: 'Clear display state', exact: true }),
  ).toBeDisabled();
  await expect(
    page.getByText(/Display maintenance requires display automation or a virtual display/).first(),
  ).toBeVisible();
  expect(calls.displayResets).toBe(0);
  expect(calls.goldenExports).toBe(0);
  expect(calls.configPatches).toEqual([]);
});

for (const available of [false, true]) {
  test(`display settings retry restores ${available ? 'available' : 'disabled'} maintenance after repeated failures`, async ({
    page,
  }) => {
    const calls = await setupHost(page, {
      golden: { maintenance_available: available },
      config: {
        dd_configuration_option: available ? 'ensure_active' : 'disabled',
        virtual_display_mode: 'disabled',
      },
    });
    calls.failGoldenStatus = true;
    await page.goto('/v2/settings?category=display');
    const retry = page.getByRole('button', { name: 'Retry status', exact: true });
    const reset = page.getByRole('button', { name: 'Clear display state', exact: true });
    const unavailableMessage = page.getByText(
      'Display maintenance status is unavailable. Refresh the status before using recovery actions.',
      { exact: true },
    );
    await expect(retry).toBeEnabled();
    await expect(reset).toBeDisabled();
    await expect(unavailableMessage).toHaveCount(2);

    const initialReads = calls.goldenStatusReads;
    await retry.click();
    await expect.poll(() => calls.goldenStatusReads).toBe(initialReads + 1);
    await expect(retry).toBeEnabled();
    await expect(reset).toBeDisabled();

    calls.failGoldenStatus = false;
    await retry.click();
    await expect.poll(() => calls.goldenStatusReads).toBe(initialReads + 2);
    const capture = page.getByRole('button', { name: 'Create snapshot', exact: true });
    if (available) {
      await expect(capture).toBeEnabled();
      await expect(reset).toBeEnabled();
    } else {
      await expect(capture).toBeDisabled();
      await expect(reset).toBeDisabled();
      await expect(
        page.getByText(/Display maintenance requires display automation or a virtual display/),
      ).toHaveCount(2);
    }
    await expect(unavailableMessage).toHaveCount(0);
    expect(calls.goldenExports).toBe(0);
    expect(calls.goldenDeletes).toBe(0);
    expect(calls.displayResets).toBe(0);
    expect(calls.configPatches).toEqual([]);
  });
}

test('failed maintenance refresh clears previously available recovery actions', async ({
  page,
}) => {
  const calls = await setupHost(page, {
    golden: { maintenance_available: true, restore_task_state: 'enabled', helper_engine: 'v2' },
  });
  await page.goto('/v2/maintenance');
  await expect(page.getByRole('button', { name: 'Capture snapshot', exact: true })).toBeEnabled();
  calls.failGoldenStatus = true;
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await expect(page.getByRole('button', { name: 'Capture snapshot', exact: true })).toBeDisabled();
  await expect(
    page.getByText(
      'Display maintenance status is unavailable. Refresh the status before using recovery actions.',
    ),
  ).toBeVisible();
  await expect(page.getByText(/Session recovery: current snapshot/)).toHaveCount(0);
  expect(calls.goldenExports).toBe(0);
  expect(calls.configPatches).toEqual([]);
});

for (const initiallyAvailable of [false, true]) {
  test(`display recovery follows saved automation ${initiallyAvailable ? 'on-off-on' : 'off-on-off'} without reload`, async ({
    page,
  }) => {
    const calls = await setupHost(page, {
      golden: { maintenance_available: initiallyAvailable },
      config: {
        dd_configuration_option: initiallyAvailable ? 'ensure_active' : 'disabled',
        virtual_display_mode: 'disabled',
      },
    });
    await page.goto('/v2/settings?category=display');
    await page.evaluate(() => {
      Object.assign(window, { recoveryPageInstance: 'same-document' });
    });
    const capture = page.getByRole('button', { name: 'Create snapshot', exact: true });
    const reset = page.getByRole('button', { name: 'Clear display state', exact: true });
    const disabledMessage = page
      .locator('.display-recovery-settings')
      .getByText(/Display maintenance requires display automation or a virtual display/);
    const assertAvailability = async (available: boolean) => {
      if (available) {
        await expect(capture).toBeEnabled();
        await expect(reset).toBeEnabled();
        await expect(disabledMessage).toHaveCount(0);
      } else {
        await expect(capture).toBeDisabled();
        await expect(reset).toBeDisabled();
        await expect(disabledMessage).toBeVisible();
      }
    };
    await assertAvailability(initiallyAvailable);
    for (const available of [!initiallyAvailable, initiallyAvailable]) {
      await page
        .locator('#setting-dd_configuration_option')
        .selectOption(available ? 'ensure_active' : 'disabled');
      // Unsaved settings must not change actions backed by the active host policy.
      await assertAvailability(!available);
      await page.getByRole('button', { name: 'Save changes', exact: true }).click();
      await assertAvailability(available);
    }
    expect(calls.configPatches).toEqual([
      { dd_configuration_option: initiallyAvailable ? 'disabled' : 'ensure_active' },
      { dd_configuration_option: initiallyAvailable ? 'ensure_active' : 'disabled' },
    ]);
    expect(calls.goldenExports).toBe(0);
    expect(calls.displayResets).toBe(0);
    expect(
      await page.evaluate(
        () => (window as Window & { recoveryPageInstance?: string }).recoveryPageInstance,
      ),
    ).toBe('same-document');
  });
}
