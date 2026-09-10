import { expect, test, type Page } from '@playwright/test';

type DriverStatus =
  | 'ready'
  | 'failed'
  | 'uninitialized'
  | 'version_incompatible'
  | 'watchdog_failed'
  | 'unknown';

async function setupWindowsHost(
  page: Page,
  options: {
    driverStatus?: DriverStatus | number;
    activeDriver?: 'vibeshine' | 'sudovda' | null;
    configuredDriver?: 'vibeshine' | 'sudovda';
    config?: Record<string, unknown>;
  } = {},
) {
  const config = {
    capture: 'wgc',
    virtual_display_mode: 'per_client',
    dd_use_sunshine_virtual_display_driver: options.configuredDriver !== 'sudovda',
    dd_wa_dummy_plug_hdr10: false,
    frame_limiter_disable_vsync: false,
    ...options.config,
  };

  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
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
    } else if (path === '/api/config' && request.method() === 'GET') {
      body = { status: true, ...config };
    } else if (path === '/api/config' && request.method() === 'PATCH') {
      Object.assign(config, request.postDataJSON());
      body = { status: true };
    } else if (path === '/api/metadata') {
      body = {
        platform: 'windows',
        windows_build_number: 26100,
        encoder_status: { state: 'ready', h264: true },
        virtual_display_driver: {
          configured: options.configuredDriver ?? 'vibeshine',
          active: options.activeDriver === undefined ? 'vibeshine' : options.activeDriver,
          status: options.driverStatus ?? 'unknown',
          status_code: typeof options.driverStatus === 'number' ? options.driverStatus : undefined,
        },
      };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false };
    } else if (path === '/api/display-devices') {
      body = [];
    }
    await route.fulfill({ json: body });
  });
}

for (const [name, status] of [
  ['incompatible', 'version_incompatible'],
  ['watchdog', 'watchdog_failed'],
  ['unknown', 'unknown'],
] as const) {
  test(`Windows display driver ${name} fixture remains actionable`, async ({ page }) => {
    await setupWindowsHost(page, { driverStatus: status });
    await page.goto('/v2/settings?category=display');
    await expect(page.locator('#windows-display-status')).toBeVisible();
    await expect(page.locator('#windows-display-status .vs-status-badge')).toContainText(
        name === 'incompatible'
        ? 'Driver version mismatch'
        : name === 'watchdog'
          ? 'Watchdog failed'
          : 'Unknown',
    );
    await expect(page.locator('#windows-display-status')).toContainText(
      'Observed by host: Vibepollo',
    );
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/windows-driver-${name}.png`,
      fullPage: true,
    });
  });
}

test('Windows display status keeps observed and configured drivers distinct', async ({ page }) => {
  await setupWindowsHost(page, {
    activeDriver: 'sudovda',
    configuredDriver: 'vibeshine',
    driverStatus: 'ready',
  });
  await page.goto('/v2/settings?category=display');
  await expect(page.locator('#windows-display-status')).toContainText('Observed by host: SudoVDA');
  await expect(page.locator('#windows-display-status')).toContainText('Settings select Vibepollo');
});

test('active dummy-plug HDR dependency is visible on desktop and narrow layouts', async ({
  page,
}) => {
  await setupWindowsHost(page, {
    config: {
      virtual_display_mode: 'disabled',
      dd_wa_dummy_plug_hdr10: true,
      frame_limiter_disable_vsync: false,
    },
  });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/settings?q=VSYNC');
  await expect(page.locator('#setting-dd_wa_dummy_plug_hdr10')).toBeChecked();
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).toBeChecked();
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).toBeDisabled();
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/windows-dummy-hdr-desktop.png',
    fullPage: true,
  });

  await page.setViewportSize({ width: 390, height: 1000 });
  await page.screenshot({
    path: '/tmp/vibeshine-ui-results/windows-dummy-hdr-narrow.png',
    fullPage: true,
  });
});

test('persisted forced VSYNC is not dirty on load and returns to its saved value when disabled', async ({
  page,
}) => {
  const patches: Record<string, unknown>[] = [];
  await setupWindowsHost(page, {
    config: {
      virtual_display_mode: 'disabled',
      dd_wa_dummy_plug_hdr10: true,
      frame_limiter_disable_vsync: false,
    },
  });
  await page.route('**/api/config', async (route) => {
    if (route.request().method() === 'PATCH') {
      patches.push(route.request().postDataJSON() as Record<string, unknown>);
    }
    await route.fallback();
  });
  await page.goto('/v2/settings?q=VSYNC');
  await expect(page.locator('.save-bar')).toHaveCount(0);
  await page.locator('#setting-dd_wa_dummy_plug_hdr10').uncheck();
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).not.toBeChecked();
  await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toBeEnabled();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ dd_wa_dummy_plug_hdr10: false });
  await page.reload();
  await expect(page.locator('.save-bar')).toHaveCount(0);
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).not.toBeChecked();
});

test('dummy-plug dependency survives disable and only changed settings are saved', async ({
  page,
}) => {
  const config = {
    virtual_display_mode: 'disabled',
    dd_wa_dummy_plug_hdr10: false,
    frame_limiter_disable_vsync: false,
  };
  const patches: Record<string, unknown>[] = [];
  await setupWindowsHost(page, { config });
  await page.route('**/api/config', async (route) => {
    if (route.request().method() === 'PATCH') {
      const patch = route.request().postDataJSON() as Record<string, unknown>;
      patches.push(patch);
      Object.assign(config, patch);
      await route.fulfill({ json: { status: true } });
      return;
    }
    await route.fallback();
  });
  await page.goto('/v2/settings?q=VSYNC');
  await page.locator('#setting-frame_limiter_disable_vsync').check();
  await page.locator('#setting-dd_wa_dummy_plug_hdr10').check();
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).toBeChecked();
  await page.locator('#setting-dd_wa_dummy_plug_hdr10').uncheck();
  await expect(page.locator('#setting-frame_limiter_disable_vsync')).toBeChecked();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ frame_limiter_disable_vsync: true });
});
