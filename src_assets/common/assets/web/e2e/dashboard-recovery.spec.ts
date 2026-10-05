import { expect, test, type Page } from '@playwright/test';

async function host(
  page: Page,
  platform = 'linux',
  result: unknown = {
    status: true,
    virtual_displays_removed: true,
    topology_restored: true,
    physical_display_recovered: true,
  },
) {
  const calls: Array<{ body: unknown; csrf: string | undefined }> = [];
  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }
    let body: unknown = { status: true };
    if (path === '/api/auth/status') {
      body = { authenticated: true, credentials_configured: true, login_required: false };
    } else if (path === '/api/configLocale') body = { locale: 'en' };
    else if (path === '/api/csrf-token') body = { csrf_token: 'recovery-test-token' };
    else if (path === '/api/metadata') {
      body = { platform, version: '2.0.1-beta.1', encoder_status: { state: 'ready', h264: true } };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 1, appRunning: true };
    } else if (path === '/api/host/info') body = { cpu_model: 'CPU', gpu_model: 'GPU' };
    else if (path === '/api/host/stats') body = { cpu_percent: 0, gpu_percent: 0 };
    else if (path === '/api/display/terminate_virtual') {
      expect(request.method()).toBe('POST');
      calls.push({ body: request.postDataJSON(), csrf: request.headers()['x-csrf-token'] });
      await route.fulfill({ body: JSON.stringify(result), contentType: 'application/json' });
      return;
    }
    await route.fulfill({ json: body });
  });
  return calls;
}

async function confirmRecovery(page: Page) {
  await page.getByRole('button', { name: 'Terminate virtual screen', exact: true }).click();
  await expect(page.getByRole('dialog')).toContainText('Streams using them will lose video.');
  await page
    .getByRole('dialog')
    .getByRole('button', { name: 'Terminate virtual screen', exact: true })
    .click();
}

for (const platform of ['linux', 'windows']) {
  test(`${platform} dashboard introduces recovery and preserves the action after dismissal`, async ({
    page,
  }) => {
    const calls = await host(page, platform);
    await page.goto('/v2/');
    await expect(
      page.getByText('Know how to recover your PC screen', { exact: true }),
    ).toBeVisible();
    await page.getByRole('button', { name: 'Got it', exact: true }).click();
    await page.reload();
    await expect(
      page.getByRole('button', { name: 'Terminate virtual screen', exact: true }),
    ).toBeVisible();
    await expect(page.getByText('Know how to recover your PC screen', { exact: true })).toHaveCount(
      0,
    );
    expect(calls).toHaveLength(0);
    await confirmRecovery(page);
    await expect(page.getByRole('status')).toContainText(
      'saved physical display layout was restored',
    );
    expect(calls).toEqual([{ body: {}, csrf: 'recovery-test-token' }]);
  });
}

test('cancelling the recovery dialog never sends a termination request', async ({ page }) => {
  const calls = await host(page);
  await page.goto('/v2/');
  await page.getByRole('button', { name: 'Terminate virtual screen', exact: true }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Cancel', exact: true }).click();
  await expect(page.getByRole('dialog')).not.toBeVisible();
  expect(calls).toHaveLength(0);
});

test('unsupported hosts do not advertise display termination', async ({ page }) => {
  await host(page, 'macos');
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Host performance', exact: true })).toBeVisible();
  await expect(
    page.getByRole('button', { name: 'Terminate virtual screen', exact: true }),
  ).toHaveCount(0);
});

const responses = [
  {
    name: 'fallback recovery',
    result: { status: true, topology_restored: false, physical_display_recovered: true },
    message: 'a physical monitor is active',
  },
  {
    name: 'failed physical recovery',
    result: { status: true, topology_restored: false, physical_display_recovered: false },
    message: 'physical-display recovery could not be confirmed',
  },
  {
    name: 'unconfirmed helper dispatch',
    result: { status: true, restore_dispatched: true },
    message: 'physical-display recovery was requested',
  },
  {
    name: 'termination failure',
    result: { status: false, physical_display_recovered: true },
    message: 'termination could not be confirmed',
  },
  { name: 'missing confirmation', result: {}, message: 'termination could not be confirmed' },
  { name: 'empty response', result: null, message: 'termination could not be confirmed' },
  {
    name: 'contradictory removal result',
    result: { status: true, virtual_displays_removed: false },
    message: 'termination could not be confirmed',
  },
];
for (const { name, result, message } of responses) {
  test(`dashboard reports ${name} separately from virtual-screen removal`, async ({ page }) => {
    const calls = await host(page, 'linux', result);
    await page.goto('/v2/');
    await confirmRecovery(page);
    await expect(page.getByRole('status')).toContainText(message);
    expect(calls).toHaveLength(1);
  });
}

test('recovery is available even when dashboard metrics fail', async ({ page }) => {
  const calls = await host(page);
  await page.route('**/api/host/stats', (route) =>
    route.fulfill({ status: 503, json: { error: 'unavailable' } }),
  );
  await page.goto('/v2/');
  await confirmRecovery(page);
  await expect(page.getByRole('status')).toContainText(
    'saved physical display layout was restored',
  );
  expect(calls).toHaveLength(1);
});

test('network failures leave recovery available for another attempt', async ({ page }) => {
  const calls = await host(page);
  await page.route('**/api/display/terminate_virtual', (route) => route.abort());
  await page.goto('/v2/');
  await confirmRecovery(page);
  await expect(page.getByRole('status')).toContainText('termination could not be confirmed');
  await page.unroute('**/api/display/terminate_virtual');
  await confirmRecovery(page);
  await expect(page.getByRole('status')).toContainText(
    'saved physical display layout was restored',
  );
  expect(calls).toHaveLength(1);
});

test('an in-flight recovery cannot be cancelled or submitted twice', async ({ page }) => {
  await host(page);
  let count = 0;
  let finish: (() => void) | undefined;
  const pending = new Promise<void>((resolve) => {
    finish = resolve;
  });
  await page.route('**/api/display/terminate_virtual', async (route) => {
    count += 1;
    await pending;
    await route.fulfill({ json: { status: true, physical_display_recovered: true } });
  });
  await page.goto('/v2/');
  await confirmRecovery(page);
  await expect(
    page.getByRole('dialog').getByRole('button', { name: 'Recovering displays…' }),
  ).toBeDisabled();
  await expect(
    page.getByRole('dialog').getByRole('button', { name: 'Cancel', exact: true }),
  ).toBeDisabled();
  await page.keyboard.press('Escape');
  await expect(page.getByRole('dialog')).toBeVisible();
  expect(count).toBe(1);
  finish?.();
  await expect(page.getByRole('status')).toContainText('a physical monitor is active');
});
