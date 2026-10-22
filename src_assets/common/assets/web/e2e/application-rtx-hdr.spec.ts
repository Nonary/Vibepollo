import { test, expect, type Page } from '@playwright/test';

const uuid = '22222222-2222-4222-8222-222222222222';
const name = 'Linux HDR game';

async function mockHost(page: Page, platform = 'linux', nvidia = true) {
  let app = {
    uuid,
    name,
    cmd: '/usr/bin/game',
    'config-overrides': { rtx_hdr: true, rtx_hdr_peak_brightness: 1000 },
  };
  const live: Array<Record<string, any>> = [];
  const saved: Array<Record<string, any>> = [];
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }
    let body: unknown = { status: true };
    if (path === '/api/auth/status')
      body = { authenticated: true, login_required: false, credentials_configured: true };
    else if (path === '/api/csrf-token') body = { csrf_token: 'test-token' };
    else if (path === '/api/configLocale') body = { locale: 'en' };
    else if (path === '/api/metadata')
      body = { platform, has_nvidia_gpu: nvidia, version: '2.0.2' };
    else if (path === '/api/config')
      body = { capture: 'kms', encoder: 'nvenc', rtx_hdr_peak_brightness: 1000 };
    else if (path === '/api/apps/rtx_hdr/live') {
      live.push(route.request().postDataJSON());
      body = { status: true, applied: true };
    } else if (path === '/api/apps' && route.request().method() === 'POST') {
      app = route.request().postDataJSON();
      saved.push(app);
    } else if (path === '/api/apps') body = { apps: [app] };
    else if (path.endsWith('/games') || path === '/api/display-devices') body = [];
    else if (path === '/api/auth/sessions') body = { sessions: [] };
    else if (path === '/api/playnite/status' || path === '/api/steam/status')
      body = { installed: false, enabled: false, active: false };
    await route.fulfill({ json: body });
  });
  return { live, saved };
}

async function open(page: Page, ui: 'classic' | 'v2') {
  if (ui === 'v2') await page.goto(`/v2/library/${uuid}`);
  else {
    await page.goto('http://127.0.0.1:5174/applications');
    await page.getByRole('button', { name, exact: false }).click();
  }
}

for (const ui of ['classic', 'v2'] as const) {
  test(`${ui} exposes Linux NVIDIA RTX HDR, sends live tuning and persists it`, async ({
    page,
  }) => {
    const { live, saved } = await mockHost(page);
    await open(page, ui);
    const section =
      ui === 'v2'
        ? page.locator('section[aria-labelledby="rtx-hdr-heading"]')
        : page
            .locator('section')
            .filter({ has: page.getByRole('heading', { name: 'RTX HDR', exact: true }) });
    await expect(section).toBeVisible();
    const peak =
      ui === 'v2'
        ? section
            .locator('article')
            .filter({ has: page.locator('#app-rtx-hdr-rtxHdrPeakBrightness') })
            .locator('input[type="number"]')
        : section.locator('#rtx_hdr_peak_brightness input');
    await peak.fill('1500');
    await peak.blur();
    await expect.poll(() => live.at(-1)?.['config-overrides']?.rtx_hdr_peak_brightness).toBe(1500);
    await page
      .getByRole('button', { name: ui === 'v2' ? 'Save application' : /Save$/, exact: true })
      .last()
      .click();
    await expect.poll(() => saved.at(-1)?.['config-overrides']?.rtx_hdr_peak_brightness).toBe(1500);
  });

  for (const [platform, nvidia] of [
    ['linux', false],
    ['macos', true],
  ] as const) {
    test(`${ui} hides RTX HDR on ${platform} with NVIDIA=${nvidia}`, async ({ page }) => {
      await mockHost(page, platform, nvidia);
      await open(page, ui);
      await expect(page.getByRole('heading', { name: /^RTX HDR/ })).toHaveCount(0);
    });
  }

  test(`${ui} exposes Linux global RTX HDR tuning`, async ({ page }) => {
    await mockHost(page);
    if (ui === 'v2') {
      await page.goto('/v2/settings?category=video');
      const section = page.locator('#settings-group-video-video_rtx_hdr');
      await expect(section).toBeVisible();
      await section.locator('summary').click();
      await expect(section.getByText('Peak Brightness (nits)', { exact: true })).toBeVisible();
    } else {
      await page.goto('http://127.0.0.1:5174/settings?sec=capture');
      await expect(
        page.getByRole('heading', { name: 'RTX HDR (AI SDR → HDR)', exact: true }),
      ).toBeVisible();
    }
  });
}
