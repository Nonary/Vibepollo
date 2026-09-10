import { test, expect, type Page } from '@playwright/test';

const appUuid = '11111111-1111-4111-8111-111111111111';

interface HostOptions {
  platform?: 'windows' | 'linux';
  apps?: Array<Record<string, unknown>>;
}

async function setupHost(page: Page, options: HostOptions = {}) {
  const platform = options.platform ?? 'windows';
  let apps = [...(options.apps ?? [])];
  const saves: Record<string, unknown>[] = [];

  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname;
    const method = request.method();
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }

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
        windows_build_number: platform === 'windows' ? 26100 : undefined,
        has_nvidia_gpu: platform === 'windows',
      };
    } else if (path === '/api/apps' && method === 'GET') {
      body = { apps };
    } else if (path === '/api/apps' && method === 'POST') {
      const saved = request.postDataJSON() as Record<string, unknown>;
      saves.push(saved);
      apps = [saved];
      body = { status: true };
    } else if (path === '/api/playnite/status') {
      body = { installed: true, active: false };
    } else if (path === '/api/steam/status') {
      body = { enabled: true, available: true };
    } else if (path === '/api/steam/games' || path === '/api/playnite/games') {
      body = [];
    } else if (path === '/api/lutris/games') {
      body = { enabled: false, games: [] };
    } else if (path === '/api/config') {
      body = { capture: 'wgc' };
    } else if (path === '/api/rtss/status') {
      body = { path_exists: true, hooks_found: true };
    } else if (path === '/api/display-devices') {
      body = [];
    }
    await route.fulfill({ json: body });
  });
  await page.route('**/assets/changelog.json', (route) =>
    route.fulfill({ json: { releases: [] } }),
  );
  return saves;
}

test('new application keeps v1 lifecycle defaults and saves launch controls', async ({ page }) => {
  const saves = await setupHost(page);
  await page.goto('/v2/library/new');
  await expect(page.locator('#app-auto-detach')).toBeChecked();
  await expect(page.locator('#app-wait-all')).toBeChecked();
  await expect(page.locator('#app-exit-timeout')).toHaveValue('5');
  await page.locator('#app-name').fill('Parity test app');
  await page.getByRole('button', { name: 'Save application', exact: true }).last().click();
  await expect.poll(() => saves.length).toBe(1);
  expect(saves[0]).toMatchObject({
    name: 'Parity test app',
    'auto-detach': true,
    'wait-all': true,
    'exclude-global-prep-cmd': false,
    'exit-timeout': 5,
  });
});

test('existing application without lifecycle keys gets v1 defaults and saves changed controls', async ({
  page,
}) => {
  const saves = await setupHost(page, {
    apps: [{ uuid: appUuid, name: 'Absent-key app', cmd: 'C:\\Games\\game.exe' }],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await expect(page.locator('#app-auto-detach')).toBeChecked();
  await expect(page.locator('#app-wait-all')).toBeChecked();
  await expect(page.locator('#app-exit-timeout')).toHaveValue('5');

  await page.locator('#app-auto-detach').uncheck();
  await page.locator('#app-wait-all').uncheck();
  await page.locator('#app-exclude-global-prep').check();
  await page.locator('#app-elevated').check();
  await page.locator('#app-exit-timeout').fill('0');
  await page.getByRole('button', { name: 'Save application', exact: true }).last().click();
  await expect.poll(() => saves.length).toBe(1);
  expect(saves[0]).toMatchObject({
    'auto-detach': false,
    'wait-all': false,
    'exclude-global-prep-cmd': true,
    elevated: true,
    'exit-timeout': 0,
  });
});

test('existing application preserves explicit false and zero, and saves both LS profiles', async ({
  page,
}) => {
  const saves = await setupHost(page, {
    apps: [
      {
        uuid: appUuid,
        name: 'Profile parity app',
        cmd: 'C:\\Games\\game.exe',
        'auto-detach': false,
        'wait-all': false,
        'exit-timeout': 0,
        'lossless-scaling-enabled': true,
        'frame-generation-mode': 'lossless-scaling',
        'lossless-scaling-target-fps': 120,
        'lossless-scaling-rtss-limit': 60,
        'lossless-scaling-profile': 'custom',
        'lossless-scaling-recommended': {
          'performance-mode': true,
          'flow-scale': 55,
          'future-profile-field': 'preserve-me-too',
        },
        'lossless-scaling-custom': {
          'scaling-type': 'ls1',
          'resolution-scale': 80,
          sharpening: 7,
        },
        'future-field': 'preserve-me',
      },
    ],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await expect(page.locator('#app-auto-detach')).not.toBeChecked();
  await expect(page.locator('#app-wait-all')).not.toBeChecked();
  await expect(page.locator('#app-exit-timeout')).toHaveValue('0');
  await expect(page.locator('#app-lossless-scaling-mode')).toHaveValue('ls1');
  await page.getByRole('button', { name: 'Percent', exact: true }).click();
  await expect(page.locator('#app-lossless-resolution-percent')).toHaveValue('80');
  await page.getByRole('button', { name: 'Scale Factor', exact: true }).click();
  await expect(page.locator('#app-lossless-resolution-factor')).toHaveValue('1.25');
  await expect(page.locator('#app-lossless-sharpening')).toHaveValue('7');

  await page.locator('#app-lossless-enabled').uncheck();
  await expect(page.locator('#app-lossless-scaling-mode')).toHaveValue('ls1');
  await page.locator('#app-lossless-enabled').check();
  await page.locator('input[type="radio"][value="recommended"]').check();
  await expect(page.locator('#app-lossless-flow-scale')).toHaveValue('55');
  await page.locator('input[type="radio"][value="custom"]').check();
  await expect(page.locator('#app-lossless-flow-scale')).toHaveValue('50');
  await expect(page.locator('#app-lossless-scaling-mode')).toHaveValue('ls1');
  await page.getByRole('button', { name: 'Reset to Profile Defaults' }).click();
  await page.getByRole('button', { name: 'Save application', exact: true }).last().click();
  await expect.poll(() => saves.length).toBe(1);
  expect(saves[0]).toMatchObject({
    'auto-detach': false,
    'wait-all': false,
    'exit-timeout': 0,
    'future-field': 'preserve-me',
    'lossless-scaling-recommended': {
      'performance-mode': true,
      'flow-scale': 55,
      'future-profile-field': 'preserve-me-too',
    },
  });
  expect(Object.prototype.hasOwnProperty.call(saves[0], 'lossless-scaling-custom')).toBe(false);
});

test('Lossless Scaling preserves explicit zero numeric overrides', async ({ page }) => {
  const saves = await setupHost(page, {
    apps: [
      {
        uuid: appUuid,
        name: 'Zero override app',
        'lossless-scaling-enabled': true,
        'frame-generation-mode': 'lossless-scaling',
        'lossless-scaling-custom': { 'flow-scale': 25 },
        'lossless-scaling-profile': 'custom',
      },
    ],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await page.locator('#app-lossless-flow-scale').fill('0');
  await page.locator('#app-lossless-target-fps').fill('0');
  await page.locator('#app-lossless-launch-delay').fill('0');
  await page.getByRole('button', { name: 'Save application', exact: true }).last().click();
  await expect.poll(() => saves.length).toBe(1);
  expect(saves[0]).toMatchObject({
    'lossless-scaling-target-fps': 0,
    'lossless-scaling-launch-delay': 0,
    'lossless-scaling-custom': { 'flow-scale': 0 },
  });
});

test('managed applications keep provider gating', async ({ page }) => {
  await setupHost(page, {
    apps: [
      {
        uuid: appUuid,
        name: 'Managed app',
        'playnite-id': 'managed-game',
        'playnite-managed': 'auto',
      },
    ],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await expect(page.locator('#app-auto-detach')).toHaveCount(0);
  await expect(page.locator('#app-wait-all')).toHaveCount(0);
  await expect(page.locator('#app-elevated')).toHaveCount(0);
  await expect(page.locator('#app-exclude-global-prep')).toBeVisible();
  await expect(page.locator('#app-exit-timeout')).toHaveValue('10');
});

test('Linux applications hide Windows-only controls', async ({ page }) => {
  await setupHost(page, {
    platform: 'linux',
    apps: [{ uuid: appUuid, name: 'Linux app' }],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await expect(page.locator('#app-auto-detach')).toBeVisible();
  await expect(page.locator('#app-elevated')).toHaveCount(0);
  await expect(page.locator('.lossless-editor')).toHaveCount(0);
});

test('Lossless Scaling controls remain usable at desktop and mobile widths', async ({ page }) => {
  await setupHost(page, {
    apps: [
      {
        uuid: appUuid,
        name: 'Responsive LS app',
        'lossless-scaling-enabled': true,
        'frame-generation-mode': 'lossless-scaling',
      },
    ],
  });
  await page.goto(`/v2/library/${appUuid}`);
  await expect(page.getByRole('heading', { name: 'Lossless Scaling', exact: true })).toBeVisible();
  const losslessPanel = page.locator('.lossless-editor');
  const launchGroup = page.locator('.lossless-editor__group').last();
  await losslessPanel.scrollIntoViewIfNeeded();
  await page.screenshot({
    path: '/tmp/vibeshine-application-review/application-parity-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await launchGroup.scrollIntoViewIfNeeded();
  await page.screenshot({
    path: '/tmp/vibeshine-application-review/application-parity-desktop-launch.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await expect(page.locator('html')).toHaveAttribute('lang', 'en');
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await losslessPanel.scrollIntoViewIfNeeded();
  const panelBox = await losslessPanel.boundingBox();
  expect(panelBox).not.toBeNull();
  expect(panelBox!.x).toBeGreaterThanOrEqual(0);
  expect(panelBox!.x + panelBox!.width).toBeLessThanOrEqual(390);
  expect(
    await losslessPanel.evaluate((element) => element.scrollWidth <= element.clientWidth),
  ).toBe(true);
  await page.screenshot({
    path: '/tmp/vibeshine-application-review/application-parity-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
  await launchGroup.scrollIntoViewIfNeeded();
  await page.screenshot({
    path: '/tmp/vibeshine-application-review/application-parity-mobile-launch.png',
    animations: 'disabled',
    fullPage: false,
  });
});
