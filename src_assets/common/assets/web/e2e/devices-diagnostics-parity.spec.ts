import { expect, test, type Page } from '@playwright/test';

type FixtureOptions = {
  platform?: 'windows' | 'linux' | 'macos';
  vigem?:
    | { status?: unknown; installed?: unknown; version?: string }
    | 'error'
    | 'status-false'
    | 'malformed';
  controller?: unknown;
};

const devices = [
  {
    uuid: 'alpha',
    name: 'Living room',
    enabled: true,
    connected: true,
    last_seen: 1_788_955_190,
    display_mode: '1920x1080x60',
    hdr_profile: 'Living HDR',
    output_name_override: 'HDMI-A-1',
  },
  {
    uuid: 'beta',
    name: 'Bedroom',
    enabled: true,
    connected: false,
    last_seen: 1_788_955_000,
    display_mode: '2560x1440x60',
    hdr_profile: 'Bedroom HDR',
    virtual_display_mode: 'per_client',
  },
  {
    uuid: 'blocked-connected',
    name: 'Blocked pad',
    enabled: false,
    connected: true,
    last_seen: 1_788_955_100,
    display_mode: '1280x720x60',
  },
  {
    uuid: 'unnamed',
    name: null,
    enabled: true,
    connected: false,
    last_seen: null,
  },
];

async function installFixture(page: Page, options: FixtureOptions = {}) {
  const platform = options.platform ?? 'windows';
  const calls = { vigem: 0 };

  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname;
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
      body = { csrf_token: 'fixture-token' };
    } else if (path === '/api/config') {
      body = {
        status: true,
        controller: options.controller ?? 'enabled',
        capture: 'wgc',
        virtual_display_mode: 'per_client',
        virtual_display_layout: 'exclusive',
      };
    } else if (path === '/api/metadata') {
      body = {
        status: true,
        platform,
        version: 'fixture',
        encoder_status: { state: 'ready', h264: true },
        capture_status: {
          configured_backend: 'kms',
          observed_backend: 'kms',
          managed_event_driven: true,
          virtual_display_configured: true,
        },
        virtual_display: { capable: true, ready: true },
      };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false };
    } else if (path === '/api/host/stats') {
      body = {
        cpu_percent: 20,
        gpu_percent: 30,
        ram_percent: 40,
        vram_percent: 50,
        ram_used_bytes: 1,
        ram_total_bytes: 2,
        vram_used_bytes: 1,
        vram_total_bytes: 2,
      };
    } else if (path === '/api/host/info') {
      body = { cpu_model: 'Fixture CPU', gpu_model: 'Fixture GPU' };
    } else if (path === '/api/health/vigem') {
      calls.vigem += 1;
      if (options.vigem === 'error') {
        await route.fulfill({ status: 503, json: { error: 'diagnostic unavailable' } });
        return;
      }
      if (options.vigem === 'status-false') {
        body = { status: false, installed: false, version: '1.16.0' };
      } else if (options.vigem === 'malformed') {
        body = { status: true, installed: 'false', version: '1.16.0' };
      } else {
        body = options.vigem ?? { installed: true, version: '1.21.442.0' };
      }
    } else if (path === '/api/clients/list') {
      body = { status: true, platform, named_certs: devices };
    } else if (path === '/api/clients/hdr-profiles') {
      body = { status: true, profiles: [] };
    } else if (path === '/api/display-devices') {
      body = [{ device_id: 'HDMI-A-1', friendly_name: 'Living room monitor' }];
    } else if (path === '/api/clients/display-layout') {
      body = { version: 1, placements: {} };
    } else if (path === '/api/apps') {
      body = { apps: [] };
    }
    await route.fulfill({ json: body });
  });

  return calls;
}

test('device filters search display metadata, sort deterministically, and keep drafts hidden by filters', async ({
  page,
}) => {
  await installFixture(page, { platform: 'linux' });
  await page.goto('/v2/devices');
  await expect(page.getByRole('heading', { name: 'Devices', exact: true })).toBeVisible();
  await expect(page.getByText('Search devices', { exact: true })).toBeVisible();
  await expect(page.locator('#device-status option[value="all"]')).toHaveText('All devices');
  await expect(page.locator('.device-row')).toHaveCount(4);

  // Recent keeps connected clients first and then uses last_seen descending.
  await expect(page.locator('.device-row h2').nth(0)).toHaveText('Living room');
  await expect(page.locator('.device-row h2').nth(1)).toHaveText('Blocked pad');

  await page.locator('#device-status').selectOption('connected');
  await expect(page.locator('.device-row')).toHaveCount(2);
  await expect(page.locator('.device-row h2').allTextContents()).resolves.toEqual([
    'Living room',
    'Blocked pad',
  ]);

  // Search covers UUID and display/HDR/routing metadata, not just the name.
  await page.locator('#device-status').selectOption('all');
  await page.locator('#device-search').fill('Bedroom HDR');
  await expect(page.locator('.device-row h2')).toHaveText('Bedroom');
  await page.locator('#device-search').fill('HDMI-A-1');
  await expect(page.locator('.device-row h2')).toHaveText('Living room');

  await page.locator('#device-search').fill('Living');
  await page.getByRole('button', { name: 'Edit device' }).click();
  await page.locator('#client-alpha-name').fill('Draft living room');
  await page.locator('#device-search').fill('');
  await page.locator('#device-status').selectOption('offline');
  await expect(page.locator('.device-row')).toHaveCount(2);
  await page.getByRole('button', { name: 'Clear filters' }).click();
  await expect(page.locator('#client-alpha-name')).toHaveValue('Draft living room');

  await page.locator('#device-sort').selectOption('status');
  await expect(page.locator('.device-row h2').allTextContents()).resolves.toEqual([
    'Blocked pad',
    'Living room',
    'Bedroom',
    'Unknown Client',
  ]);
  await page.locator('#device-sort').selectOption('name');
  await expect(page.locator('.device-row h2').allTextContents()).resolves.toEqual([
    'Bedroom',
    'Blocked pad',
    'Living room',
    'Unknown Client',
  ]);
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/devices-filters-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/devices-filters-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
});

test('Windows overview reports only an explicit missing ViGEm driver with version and link', async ({
  page,
}) => {
  const calls = await installFixture(page, {
    platform: 'windows',
    vigem: { installed: false, version: '1.16.0' },
  });
  await page.goto('/v2/');
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toBeVisible();
  await expect(page.getByText(/Detected: 1\.16\.0/)).toBeVisible();
  await expect(page.getByRole('link', { name: 'Download ViGEmBus' })).toHaveAttribute(
    'href',
    'https://github.com/nefarius/ViGEmBus/releases/latest',
  );
  expect(calls.vigem).toBeGreaterThan(0);
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/overview-vigem-desktop.png',
    animations: 'disabled',
    fullPage: false,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/overview-vigem-mobile.png',
    animations: 'disabled',
    fullPage: false,
  });
});

test('Linux overview never probes or displays the Windows ViGEm diagnostic', async ({ page }) => {
  const calls = await installFixture(page, {
    platform: 'linux',
    vigem: { installed: false, version: '1.16.0' },
  });
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream', exact: true })).toBeVisible();
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toHaveCount(0);
  expect(calls.vigem).toBe(0);
});

test('ViGEm diagnostic failure remains silent and controller false strings disable the probe', async ({
  page,
}) => {
  const failed = await installFixture(page, { platform: 'windows', vigem: 'error' });
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream', exact: true })).toBeVisible();
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toHaveCount(0);
  expect(failed.vigem).toBeGreaterThan(0);

  const statusFalse = await installFixture(page, {
    platform: 'windows',
    vigem: 'status-false',
  });
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream', exact: true })).toBeVisible();
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toHaveCount(0);
  expect(statusFalse.vigem).toBeGreaterThan(0);

  const malformed = await installFixture(page, { platform: 'windows', vigem: 'malformed' });
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream', exact: true })).toBeVisible();
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toHaveCount(0);
  expect(malformed.vigem).toBeGreaterThan(0);

  const disabled = await installFixture(page, { platform: 'windows', controller: 'false' });
  await page.goto('/v2/');
  await expect(page.getByText('Virtual Gamepad Driver (ViGEm) not installed')).toHaveCount(0);
  expect(disabled.vigem).toBe(0);
});
