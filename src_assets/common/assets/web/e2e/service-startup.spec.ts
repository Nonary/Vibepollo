import { test, expect, type Page } from '@playwright/test';

interface HostOptions {
  platform?: 'windows' | 'linux';
  automatic?: boolean;
  denied?: boolean;
  missing?: boolean;
  failSave?: boolean;
  failRead?: boolean;
  metadataDelayMs?: number;
}

async function host(page: Page, options: HostOptions = {}) {
  let automatic = options.automatic !== false;
  const calls = { reads: 0, mutations: [] as { path: string; body: unknown }[] };
  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (!path.startsWith('/api/')) return route.continue();
    let body: unknown = { status: true };
    if (request.method() === 'POST') {
      calls.mutations.push({ path, body: request.postDataJSON() });
    }
    if (path === '/api/auth/status') {
      body = { authenticated: true, login_required: false, credentials_configured: true };
    } else if (path === '/api/metadata') {
      if (options.metadataDelayMs)
        await new Promise((resolve) => setTimeout(resolve, options.metadataDelayMs));
      body = { platform: options.platform ?? 'windows', version: '2.0.1', status: true };
    } else if (path === '/api/configLocale') {
      body = { locale: 'en' };
    } else if (path === '/api/csrf-token') {
      body = { csrf_token: 'startup-test' };
    } else if (path === '/api/auth/sessions') {
      body = { sessions: [] };
    } else if (path === '/api/session/status') {
      body = { activeSessions: 0, appRunning: false };
    } else if (path === '/api/health/crashdump') {
      body = { available: false };
    } else if (path === '/api/health/vulkan-hdr-layer') {
      body = { installed: true, enabled: false };
    } else if (path === '/api/display/golden_status') {
      body = { exists: false };
    } else if (path === '/api/apps') {
      body = { apps: [] };
    } else if (path === '/api/logs') {
      return route.fulfill({ contentType: 'text/plain', body: '' });
    } else if (path === '/api/service/startup') {
      if (request.method() === 'GET') {
        calls.reads += 1;
        if (options.failRead) return route.fulfill({ status: 503, json: { error: 'Unavailable' } });
      } else if (!options.failSave) {
        automatic = (request.postDataJSON() as { automatic: boolean }).automatic;
      }
      body = {
        status: !options.missing && !(options.failSave && request.method() === 'POST'),
        installed: !options.missing,
        automatic,
        start_type: options.missing ? 'unknown' : automatic ? 'automatic' : 'manual',
        can_change: !options.denied && !options.missing,
        error: options.missing
          ? 'The Windows service is not installed.'
          : options.denied
            ? 'Administrator permissions are required to change Windows service startup.'
            : options.failSave && request.method() === 'POST'
              ? 'Windows rejected the startup change.'
              : '',
      };
    }
    await route.fulfill({ json: body });
  });
  return calls;
}

for (const [interfaceName, path] of [
  ['legacy', '/troubleshooting'],
  ['v2', '/v2/maintenance'],
]) {
  test(`${interfaceName} changes automatic startup to manual and back without restart`, async ({
    page,
  }) => {
    const calls = await host(page, { metadataDelayMs: 200 });
    await page.goto(path);
    const toggle = page.getByRole('checkbox', {
      name: 'Start automatically with Windows',
      exact: true,
    });
    await expect(toggle).toBeChecked();
    await expect(toggle).toBeEnabled();
    expect(calls.mutations).toEqual([]);
    await toggle.uncheck();
    await expect(page.getByText('Service startup mode: Manual', { exact: true })).toBeVisible();
    await expect(
      page.getByText('Startup preference saved. The current service and stream keep running.', {
        exact: true,
      }),
    ).toBeVisible();
    await toggle.check();
    await expect(page.getByText('Service startup mode: Automatic', { exact: true })).toBeVisible();
    expect(calls.mutations).toEqual([
      { path: '/api/service/startup', body: { automatic: false } },
      { path: '/api/service/startup', body: { automatic: true } },
    ]);
    await expect(
      page.getByText(/This changes future startup only; the current stream keeps running/),
    ).toBeVisible();
  });

  test(`${interfaceName} reads a manual preference without writing it`, async ({ page }) => {
    const calls = await host(page, { automatic: false });
    await page.goto(path);
    await expect(page.getByText('Service startup mode: Manual', { exact: true })).toBeVisible();
    await expect(
      page.getByRole('checkbox', { name: 'Start automatically with Windows', exact: true }),
    ).not.toBeChecked();
    expect(calls.mutations).toEqual([]);
  });

  test(`${interfaceName} shows actual mode with disabled control when access is denied`, async ({
    page,
  }) => {
    const calls = await host(page, { denied: true });
    await page.goto(path);
    const toggle = page.getByRole('checkbox', {
      name: 'Start automatically with Windows',
      exact: true,
    });
    await expect(toggle).toBeChecked();
    await expect(toggle).toBeDisabled();
    await expect(
      page.getByText('Administrator permissions are required to change Windows service startup.', {
        exact: true,
      }),
    ).toBeVisible();
    expect(calls.mutations).toEqual([]);
  });

  test(`${interfaceName} explains missing installed service`, async ({ page }) => {
    const calls = await host(page, { missing: true });
    await page.goto(path);
    await expect(
      page.getByRole('checkbox', { name: 'Start automatically with Windows', exact: true }),
    ).toBeDisabled();
    await expect(
      page.getByText('The Windows service is not installed.', { exact: true }),
    ).toBeVisible();
    await expect(
      page.getByText('Service startup mode: Unavailable', { exact: true }),
    ).toBeVisible();
    expect(calls.mutations).toEqual([]);
  });

  test(`${interfaceName} restores actual state and shows rejected save`, async ({ page }) => {
    const calls = await host(page, { failSave: true });
    await page.goto(path);
    const toggle = page.getByRole('checkbox', {
      name: 'Start automatically with Windows',
      exact: true,
    });
    await expect(toggle).toBeEnabled();
    await toggle.uncheck();
    await expect(
      page.getByText('Windows rejected the startup change.', { exact: true }),
    ).toBeVisible();
    await expect(toggle).toBeChecked();
    await expect(page.getByText('Service startup mode: Automatic', { exact: true })).toBeVisible();
    await expect(
      page.getByText('Startup preference saved. The current service and stream keep running.', {
        exact: true,
      }),
    ).toHaveCount(0);
    expect(calls.mutations).toEqual([{ path: '/api/service/startup', body: { automatic: false } }]);
    expect(calls.reads).toBeGreaterThan(1);
  });

  test(`${interfaceName} cannot change startup when status refresh fails`, async ({ page }) => {
    const calls = await host(page, { failRead: true });
    await page.goto(path);
    await expect(
      page.getByRole('checkbox', { name: 'Start automatically with Windows', exact: true }),
    ).toBeDisabled();
    await expect(
      page.getByText(
        'Unable to read or save the service startup preference. Refresh and try again.',
        { exact: true },
      ),
    ).toBeVisible();
    expect(calls.mutations).toEqual([]);
  });

  test(`${interfaceName} does not query or expose Windows startup on Linux`, async ({ page }) => {
    const calls = await host(page, { platform: 'linux' });
    await page.goto(path);
    await expect(
      page.getByRole('main').getByRole('heading', {
        name: interfaceName === 'v2' ? 'Maintenance' : 'Troubleshooting',
        exact: true,
      }),
    ).toBeVisible();
    await expect(
      page.getByRole('checkbox', { name: 'Start automatically with Windows', exact: true }),
    ).toHaveCount(0);
    expect(calls.reads).toBe(0);
    expect(calls.mutations).toEqual([]);
  });
}
