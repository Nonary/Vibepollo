import { expect, test, type Page } from '@playwright/test';

async function host(page: Page, machineHost: boolean, savedTray?: string, platform = 'linux') {
  const patches: Record<string, unknown>[] = [];
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
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
    } else if (path === '/api/metadata') {
      body = { platform, linux: { machine_host: machineHost, session_role: 'desktop' } };
    } else if (path === '/api/config') {
      if (route.request().method() === 'PATCH') {
        patches.push(route.request().postDataJSON());
      } else {
        body = {
          status: true,
          sunshine_name: 'Host',
          ...(savedTray === undefined ? {} : { system_tray: savedTray }),
        };
      }
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false };
    } else if (path === '/api/display-devices') {
      body = [];
    }
    await route.fulfill({ json: body });
  });
  return patches;
}

for (const saved of [undefined, 'enabled', 'disabled']) {
  test(`machine service shows tray unavailable with saved preference ${saved}`, async ({
    page,
  }) => {
    const patches = await host(page, true, saved);
    await page.goto('/v2/settings?q=system_tray');
    const tray = page.locator('#setting-system_tray');
    await expect(tray).toBeVisible();
    await expect(tray).toBeDisabled();
    await expect(tray).not.toBeChecked();
    await expect(
      page.getByText(/The Linux system service cannot display a tray icon/),
    ).toBeVisible();
    await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toHaveCount(0);
    if (saved === 'enabled')
      await page.screenshot({
        path: test.info().outputPath('tray-service-desktop.png'),
        fullPage: true,
      });
    await page.getByPlaceholder('Search settings').fill('sunshine_name');
    await page.locator('#setting-sunshine_name').fill('Other host');
    await page.getByRole('button', { name: 'Save changes', exact: true }).click();
    await expect.poll(() => patches.length).toBe(1);
    expect(patches[0]).toEqual({ sunshine_name: 'Other host' });
  });
}

for (const platform of ['linux', 'windows']) {
  test(`standalone ${platform} keeps its saved tray setting editable`, async ({ page }) => {
    await host(page, false, 'enabled', platform);
    await page.goto('/v2/settings?q=system_tray');
    const tray = page.locator('#setting-system_tray');
    await expect(tray).toBeEnabled();
    await expect(tray).toBeChecked();
    await tray.uncheck();
    await expect(tray).not.toBeChecked();
    await expect(page.getByText(/The Linux system service cannot display a tray icon/)).toHaveCount(
      0,
    );
  });
}

test('machine tray explanation fits a phone viewport', async ({ page }) => {
  await host(page, true, 'enabled');
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto('/v2/settings?q=system_tray');
  await expect(page.locator('#setting-system_tray')).toBeDisabled();
  await expect(page.getByText(/The Linux system service cannot display a tray icon/)).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: test.info().outputPath('tray-service-phone.png'), fullPage: true });
});

test.describe('classic tray policy', () => {
  const legacyUrl = process.env.TRAY_LEGACY_TEST_URL;
  test.skip(!legacyUrl, 'Set TRAY_LEGACY_TEST_URL to a built classic UI preview.');
  for (const machineHost of [true, false]) {
    test(`classic Linux tray respects machine-host policy ${machineHost}`, async ({ page }) => {
      const patches = await host(page, machineHost, 'enabled');
      await page.goto(`${legacyUrl}/settings`);
      const tray = page.locator('#system_tray_cb');
      await expect(tray).toBeVisible();
      await expect(tray).toHaveAttribute('aria-checked', String(!machineHost));
      if (machineHost) {
        await expect(tray).toHaveClass(/n-checkbox--disabled/);
        await tray.click({ force: true });
        await expect(tray).toHaveAttribute('aria-checked', 'false');
        await expect(
          page.getByText(/The Linux system service cannot display a tray icon/),
        ).toBeVisible();
        await page.screenshot({
          path: test.info().outputPath('tray-classic-service.png'),
          fullPage: true,
        });
      } else {
        await expect(tray).toBeEnabled();
      }
      expect(patches).toEqual([]);
    });
  }
});
