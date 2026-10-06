import { expect, test } from '@playwright/test';

test('a paused host opens the UI while display metadata is stalled and can retry', async ({ page }) => {
  let metadataReady = false;
  let metadataRequests = 0;
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }
    if (path === '/api/auth/status') {
      await route.fulfill({
        json: { authenticated: true, credentials_configured: true, login_required: false },
      });
    } else if (path === '/api/metadata') {
      metadataRequests += 1;
      if (!metadataReady) return; // Simulate a display-driver call that never completes.
      await route.fulfill({
        json: { platform: 'windows', version: '2.0.0', encoder_status: { state: 'ready', h264: true } },
      });
    } else if (path === '/api/session/status') {
      await route.fulfill({
        json: { status: true, activeSessions: 0, appRunning: true, appName: 'Desktop', paused: true },
      });
    } else if (path === '/api/configLocale') {
      await route.fulfill({ json: { locale: 'en' } });
    } else {
      await route.fulfill({ json: { status: true } });
    }
  });

  await page.goto('/v2/');
  // The shell must open before the five-second status-request timeout.
  await expect(page.getByRole('heading', { name: 'Host performance', exact: true })).toBeVisible({
    timeout: 3000,
  });
  await expect(page.locator('.boot-screen')).toHaveCount(0);
  await expect.poll(() => metadataRequests).toBe(1);

  // A failed discovery must release the refresh guard so later polling works.
  metadataReady = true;
  await expect.poll(() => metadataRequests, { timeout: 20000 }).toBeGreaterThan(1);
  await expect(page.getByText('2.0.0', { exact: true }).first()).toBeVisible();
});
