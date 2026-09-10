import { expect, test, type Page } from '@playwright/test';

async function host(page: Page, config: Record<string, unknown> = {}) {
  const patches: Record<string, unknown>[] = [];
  await page.route('**/api/**', async (route) => {
    const url = new URL(route.request().url());
    const path = url.pathname;
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }
    const method = route.request().method();
    let body: unknown = { status: true };
    if (path === '/api/auth/status') {
      body = { authenticated: true, login_required: false, credentials_configured: true };
    } else if (path === '/api/configLocale') {
      body = { locale: 'en' };
    } else if (path === '/api/csrf-token') {
      body = { csrf_token: 'test-token' };
    } else if (path === '/api/config') {
      if (method === 'PATCH') {
        const patch = route.request().postDataJSON() as Record<string, unknown>;
        patches.push(patch);
        Object.assign(config, patch);
        body = { status: true, deferred: true, restartRequired: 'port' in patch };
      } else {
        body = { status: true, ...config };
      }
    } else if (path === '/api/metadata') {
      body = {
        platform: 'linux',
        version: '1.0.0',
        encoder_status: { state: 'ready', h264: true },
        virtual_display: { capable: true, ready: true },
        capture_status: {
          configured_backend: 'kms',
          observed_backend: 'unknown',
          managed_event_driven: false,
          virtual_display_configured: true,
        },
        linux: { session_role: 'desktop' },
      };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false, lastEncoderProbeFailed: false };
    } else if (path === '/api/display-devices') {
      body = [];
    }
    await route.fulfill({ json: body });
  });
  return patches;
}

function rowPorts(page: Page) {
  return page.locator('.network-port-details__table tbody tr td:nth-child(2)');
}

test('custom network ports show all listeners, update WAN warning before save, and preserve other settings', async ({
  page,
}) => {
  const config = {
    port: 50000,
    origin_web_ui_allowed: 'lan',
    unrelated_network_setting: 'keep-me',
  };
  const patches = await host(page, config);
  await page.setViewportSize({ width: 1440, height: 1100 });
  await page.goto('/v2/settings?category=network#setting-port');

  await expect(page.locator('#setting-port')).toBeVisible();
  await expect(rowPorts(page)).toHaveText(['49995', '50000', '50001', '50021', '50009 - 50011']);
  await expect(page.getByText('Use this port to connect with Moonlight.')).toBeVisible();
  await expect(
    page.getByText('Exposing the Web UI to the internet is a security risk!'),
  ).toHaveCount(0);

  await page.locator('#setting-origin_web_ui_allowed').selectOption('wan');
  await expect(
    page.getByText('Exposing the Web UI to the internet is a security risk!'),
  ).toBeVisible();
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/network-custom-desktop-wan.png',
    fullPage: true,
  });

  await page.locator('#setting-port').fill('50001');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ port: 50001, origin_web_ui_allowed: 'wan' });
  expect(patches[0]).not.toHaveProperty('unrelated_network_setting');
});

test('network details remain readable at narrow width and contain no page overflow', async ({
  page,
}) => {
  await host(page, { port: 50000, origin_web_ui_allowed: 'wan' });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto('/v2/settings?category=network');
  await expect(rowPorts(page)).toHaveCount(5);
  await expect(
    page.locator('.network-port-details__table td[data-label="Note"]:visible'),
  ).toHaveCount(2);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await expect(
    page.getByText('Exposing the Web UI to the internet is a security risk!'),
  ).toBeVisible();
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/network-custom-mobile-wan.png',
    fullPage: true,
  });
});

test('invalid network drafts show a clear error, no derived rows, and cannot be saved', async ({
  page,
}) => {
  const patches = await host(page, { port: 47989, origin_web_ui_allowed: 'lan' });
  await page.goto('/v2/settings?category=network');
  const port = page.locator('#setting-port');

  await port.fill('1028');
  await expect(rowPorts(page)).toHaveCount(0);
  await expect(
    page.getByText('This base port would place the TCP listener at base − 5 below 1024.'),
  ).toBeVisible();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(0);

  await port.fill('1029');
  await expect(rowPorts(page)).toHaveText(['1024', '1029', '1030', '1050', '1038 - 1040']);
  await port.fill('65514');
  await expect(rowPorts(page)).toHaveText(['65509', '65514', '65515', '65535', '65523 - 65525']);
  await port.fill('65515');
  await expect(rowPorts(page)).toHaveCount(0);
  await expect(
    page.getByText('This base port would place the TCP listener at base + 21 above 65535.'),
  ).toBeVisible();
  await page.locator('.network-port-details').screenshot({
    path: '/tmp/vibeshine-ui-results/network-invalid-desktop-details.png',
  });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/network-invalid-desktop.png',
    fullPage: true,
  });

  await port.fill('');
  await expect(rowPorts(page)).toHaveCount(0);
  await expect(page.getByText('Enter a base port.')).toBeVisible();
  await port.fill('1029.5');
  await expect(rowPorts(page)).toHaveCount(0);
  await expect(page.getByText('Enter a whole number for the base port.')).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/network-invalid-mobile.png',
    fullPage: true,
  });
});

test('fractional and empty port drafts remain unsaveable after changing category', async ({
  page,
}) => {
  const patches = await host(page, { port: 47989, origin_web_ui_allowed: 'lan' });
  await page.goto('/v2/settings?category=network');
  const port = page.locator('#setting-port');

  await port.fill('1029.5');
  await page.getByRole('button', { name: 'Everyday setup', exact: true }).click();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(0);
  await expect(page.getByText('Port: Enter a whole number for the base port.')).toBeVisible();

  await page.getByRole('button', { name: 'Network & security', exact: true }).click();
  await port.fill('');
  await page.getByRole('button', { name: 'Everyday setup', exact: true }).click();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(0);
  await expect(page.getByText('Port: Enter a base port.')).toBeVisible();
});

test('network search and the port deep link retain contextual warnings', async ({ page }) => {
  await host(page, { port: 47989, origin_web_ui_allowed: 'wan' });
  await page.goto('/v2/settings?category=network#setting-port');
  await expect(page.locator('#setting-port')).toBeFocused();
  await expect(
    page.getByText('Exposing the Web UI to the internet is a security risk!'),
  ).toBeVisible();

  await page.getByRole('searchbox', { name: 'Search settings' }).fill('Moonlight');
  await expect(page.locator('#setting-port')).toBeVisible();
  await expect(page.locator('.network-port-details__table')).toBeVisible();
  await expect(
    page.getByText('Exposing the Web UI to the internet is a security risk!'),
  ).toBeVisible();
});
