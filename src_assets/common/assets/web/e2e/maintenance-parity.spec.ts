import { test, expect, type Page } from '@playwright/test';

interface HostOptions {
  platform?: 'windows' | 'linux';
  apps?: Array<Record<string, unknown>>;
  browse?: boolean;
}

async function setupHost(page: Page, options: HostOptions = {}) {
  const platform = options.platform ?? 'windows';
  const apps = options.apps ?? [];
  let currentApps = [...apps];
  const calls = {
    crashManifest: 0,
    crashParts: [] as number[],
    launch: 0,
    purgeAutosync: 0,
    appDeletes: [] as string[],
    browse: [] as string[],
    configPatches: [] as Record<string, unknown>[],
    playniteStatus: 0,
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
      body = { exists: false };
    } else if (path === '/api/apps') {
      body = { apps: currentApps };
    } else if (path === '/api/playnite/status') {
      calls.playniteStatus += 1;
      body = {
        enabled: true,
        available: true,
        active: false,
        installed: true,
        extensions_dir: 'C:\\Playnite\\Extensions',
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
      body = {
        capture: 'wgc',
        encoder: 'nvenc',
        lossless_scaling_path: '',
        lossless_scaling_legacy_auto_detect: false,
        playnite_auto_sync: true,
      };
    } else if (path === '/api/config' && method === 'PATCH') {
      calls.configPatches.push(request.postDataJSON());
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
