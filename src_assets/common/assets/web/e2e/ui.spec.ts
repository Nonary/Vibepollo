import { readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { localRoute, test, expect, type Page } from './fixtures';

const screenshotDirectory = process.env.VIBEPOLLO_TEST_OUTPUT || process.env.VIBESHINE_TEST_OUTPUT || join(tmpdir(), 'vibepollo-ui-results');

async function host(
  page: Page,
  platform = 'linux',
  config: Record<string, unknown> = {},
  ready = true,
  providers: Record<string, boolean> = { steam: true, lutris: platform === 'linux', mangohud: platform === 'linux' },
  metadata: Record<string, unknown> = {},
) {
  const patches: Record<string, unknown>[] = [];
  await localRoute(page, '**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (!path.startsWith('/api/')) {
      await route.fallback();
      return;
    }
    const method = route.request().method();
    let body: unknown = { status: true };
    if (path === '/api/auth/status')
      body = { authenticated: true, login_required: false, credentials_configured: true };
    else if (path === '/api/configLocale') body = { locale: 'en' };
    else if (path === '/api/csrf-token') body = { csrf_token: 'test-token' };
    else if (path === '/api/config') {
      if (method === 'PATCH') {
        const patch = route.request().postDataJSON();
        patches.push(patch);
        Object.assign(config, patch);
        body = { status: true, deferred: true, restartRequired: 'port' in patch };
      } else body = { status: true, capture: 'kms', virtual_display_mode: 'per_client', ...config };
    } else if (path === '/api/metadata')
      body = {
        platform,
        architecture: 'x86_64',
        providers,
        version: '1.0.0',
        encoder_status: { state: 'ready', h264: true },
        virtual_display: {
          capable: ready,
          ready,
          reason: ready ? '' : 'driver_or_outputs_unavailable',
        },
        capture_status: {
          configured_backend: 'kms',
          observed_backend: 'unknown',
          managed_event_driven: false,
          virtual_display_configured: true,
        },
        linux: { session_role: 'desktop' },
        windows_build_number: 26100,
        ...metadata,
      };
    else if (path === '/api/session/status')
      body = { status: true, activeSessions: 0, appRunning: false, lastEncoderProbeFailed: false };
    else if (path === '/api/display-devices')
      body = [{ device_id: 'HDMI-A-1', friendly_name: 'Local monitor', info: { active: true } }];
    else if (path === '/api/clients/list')
      body = {
        named_certs: [{ uuid: 'device-1', name: 'Living room', enabled: true, connected: false }],
      };
    else if (path === '/api/clients/display-layout') body = { version: 1, placements: {} };
    else if (path === '/api/apps') body = { apps: [] };
    else if (/games|categories|sessions|history/.test(path)) body = [];
    await route.fulfill({ json: body });
  });
  return patches;
}

test('Linux Everyday presents essentials and preserves unrelated values when saving', async ({
  page,
}) => {
  const patches = await host(page, 'linux', {
    nvenc_twopass: 'full_res',
    dd_manual_resolution: '2560x1440',
  });
  await page.goto('/v2/settings');
  await expect(page.getByText('Display readiness')).toBeVisible();
  await expect(page.getByText('Configured for direct capture')).toBeVisible();
  await expect(page.locator('#setting-virtual_display_mode option').first()).toHaveAttribute(
    'value',
    'per_client',
  );
  await expect(page.locator('#setting-frame_limiter_auto_virtual_framegen')).toBeVisible();
  await expect(page.locator('#setting-capture')).not.toBeVisible();
  await expect(page.locator('#setting-encoder')).not.toBeVisible();
  await expect(page.locator('#setting-fec_percentage')).not.toBeVisible();
  await expect(page.locator('#setting-dd_config_revert_on_disconnect')).not.toBeVisible();
  await expect(page.locator('#setting-controller')).toHaveCount(0);
  await page.locator('#setting-pyrowave').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ pyrowave: false });
  await page.getByRole('button', { name: 'Audio', exact: true }).click();
  await expect(page.locator('#setting-stream_audio')).toBeChecked();
  await page.getByRole('button', { name: 'Input', exact: true }).click();
  await expect(page.locator('#setting-controller')).toBeVisible();
});

test('Everyday restores impactful choices and shares drafts with detailed categories', async ({
  page,
}) => {
  const patches = await host(page, 'linux', {
    encoder: 'nvenc',
    nvenc_twopass: 'full_res',
    dd_paused_virtual_display_timeout_secs: 1234,
  });
  await page.goto('/v2/settings');
  await page.locator('#settings-group-everyday-everyday_encoding summary').click();
  await page.locator('#settings-group-everyday-everyday_recovery summary').click();
  await page.locator('#settings-group-everyday-everyday_compatibility summary').click();
  await expect(page.locator('#setting-dd_configuration_option')).toHaveCount(0);
  await expect(page.locator('#setting-dd_hdr_request_override')).toBeVisible();
  await expect(page.locator('#setting-nvenc_preset')).toBeVisible();
  await expect(page.locator('#setting-dd_paused_virtual_display_timeout_secs')).toHaveValue('1234');
  await expect(page.locator('#setting-rtss_install_path')).toHaveCount(0);
  await expect(page.locator('#setting-nvenc_twopass')).toHaveCount(0);
  await expect(page.locator('#settings-group-everyday-everyday_automation')).toBeVisible();

  await page.locator('#setting-capture').selectOption('kwin');
  await page.locator('#setting-nvenc_preset').selectOption('3');
  await page.locator('#setting-fec_percentage').fill('10');
  await page.locator('#setting-dd_config_revert_on_disconnect').check();
  await expect(page.locator('#setting-dd_paused_virtual_display_timeout_secs')).toHaveCount(0);
  await page.locator('#setting-dd_config_revert_on_disconnect').uncheck();
  await expect(page.locator('#setting-dd_paused_virtual_display_timeout_secs')).toHaveValue('1234');
  await page.locator('#setting-dd_paused_virtual_display_timeout_secs').selectOption('3600');

  await page.locator('#settings-group-everyday-everyday_encoding a.settings-more').click();
  await expect(page).toHaveURL(/category=video/);
  await page.locator('#settings-group-video-encoder_nvenc summary').click();
  await expect(page.locator('#setting-nvenc_preset')).toHaveValue('3');
  await expect(page.locator('#setting-nvenc_twopass')).toHaveValue('full_res');
  await page.getByRole('button', { name: 'Everyday setup', exact: true }).click();
  await page.locator('#settings-group-everyday-everyday_compatibility summary').click();
  await expect(page.locator('#setting-capture')).toHaveValue('kwin');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({
    capture: 'kwin',
    nvenc_preset: '3',
    fec_percentage: 10,
    dd_paused_virtual_display_timeout_secs: 3600,
  });
});

test('Everyday encoder presets follow automatic GPU selection and explicit overrides', async ({
  page,
}) => {
  await host(page, 'windows', { capture: '' }, true, undefined, {
    gpus: [{ description: 'NVIDIA GPU', vendor_id: 0x10de, dedicated_video_memory: 8192 }],
  });
  await page.goto('/v2/settings');
  await page.locator('#settings-group-everyday-everyday_encoding summary').click();
  await expect(page.locator('#setting-nvenc_preset')).toBeVisible();
  await expect(page.locator('#setting-qsv_preset')).toHaveCount(0);
  await expect(page.locator('#setting-amd_quality')).toHaveCount(0);
  await page.locator('#setting-encoder').selectOption('quicksync');
  await expect(page.locator('#setting-qsv_preset')).toBeVisible();
  await expect(page.locator('#setting-nvenc_preset')).toHaveCount(0);
  await page.locator('#setting-encoder').selectOption('amdvce_ffmpeg');
  await expect(page.locator('#setting-amd_quality')).toBeVisible();
  await expect(page.locator('#setting-qsv_preset')).toHaveCount(0);
});

test('Everyday integrates Linux pacing tools and Remote Monitor with the shared save', async ({
  page,
}) => {
  const patches = await host(page);
  await page.goto('/v2/settings');
  await expect(
    page.locator('#setting-frame_limiter_auto_virtual_framegen option[value="vrr"]'),
  ).toHaveCount(0);
  await page.locator('#settings-group-everyday-everyday_integrations summary').click();
  await expect(page.locator('#setting-rtss_install_path')).toHaveCount(0);
  await expect(page.locator('#setting-lossless_scaling_path')).toHaveCount(0);
  const libraries = page.getByRole('navigation', { name: 'Game library integrations' });
  await expect(libraries.getByRole('link', { name: 'Steam' })).toHaveAttribute(
    'href',
    '/v2/integrations#integration-steam',
  );
  await expect(libraries.getByRole('link', { name: 'Lutris' })).toBeVisible();
  await expect(libraries.getByRole('link', { name: /Playnite/ })).toHaveCount(0);
  await page.locator('#setting-frame_limiter_enable').check();
  await page.locator('#setting-frame_limiter_provider').selectOption('mangohud');
  await page.locator('#setting-mangohud_limiter_method').selectOption('early');
  await page.locator('#setting-frame_limiter_fps_limit').fill('119.88');
  await page.locator('#setting-mangohud_preset').selectOption('3');
  await page.locator('#settings-group-everyday-everyday_remote_monitor summary').click();
  await page.locator('#setting-virtual_display_max_clients').fill('6');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({
    frame_limiter_enable: true,
    frame_limiter_provider: 'mangohud',
    mangohud_limiter_method: 'early',
    frame_limiter_fps_limit: 119.88,
    mangohud_preset: '3',
    virtual_display_max_clients: 6,
  });
});

test('Everyday integrates Windows RTSS and Lossless Scaling paths', async ({ page }) => {
  const patches = await host(page, 'windows');
  await localRoute(page, '**/api/rtss/status', (route) =>
    route.fulfill({
      json: {
        path_exists: true,
        hooks_found: true,
        configured_provider: 'auto',
        resolved_path: 'C:\\RTSS',
      },
    }),
  );
  await page.goto('/v2/settings');
  await page.locator('#settings-group-everyday-everyday_integrations summary').click();
  await expect(page.locator('#setting-mangohud_preset')).toHaveCount(0);
  await expect(page.locator('#setting-rtss_install_path')).toBeVisible();
  await expect(page.locator('#setting-lossless_scaling_path')).toBeVisible();
  const playnite = page
    .getByRole('navigation', { name: 'Game library integrations' })
    .getByRole('link', { name: /Playnite/ });
  await expect(playnite).toHaveAttribute('href', '/v2/integrations#integration-playnite');
  await expect(page.locator('#setting-frame_limiter_provider option[value="proton"]')).toHaveCount(
    0,
  );
  await expect(
    page.locator('#setting-frame_limiter_provider option[value="mangohud-proton"]'),
  ).toHaveCount(0);
  await page.locator('#setting-rtss_install_path').fill('C:\\Tools\\RTSS');
  await page.locator('#setting-lossless_scaling_path').fill('C:\\Games\\LosslessScaling.exe');
  await expect(page.getByText('Saved installation ready', { exact: true })).toBeVisible();
  await expect(page.getByText(/RTSS status checks the saved installation/)).toBeVisible();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({
    rtss_install_path: 'C:\\Tools\\RTSS',
    lossless_scaling_path: 'C:\\Games\\LosslessScaling.exe',
  });
  await playnite.click();
  await expect(page).toHaveURL(/integrations#integration-playnite/);
  await expect(page.locator('#integration-playnite')).toBeVisible();
});

test('Windows identifies unsupported saved providers without silently rewriting them', async ({
  page,
}) => {
  const patches = await host(page, 'windows', { frame_limiter_provider: 'proton' });
  await page.goto('/v2/settings');
  await page.locator('#settings-group-everyday-everyday_integrations summary').click();
  await expect(page.locator('#setting-frame_limiter_provider')).toHaveValue('proton');
  await expect(page.locator('#setting-frame_limiter_provider option:checked')).toHaveText(
    'Unsupported saved value (proton)',
  );
  expect(patches).toHaveLength(0);
  await page.locator('#setting-frame_limiter_provider').selectOption('auto');
  await expect(page.locator('#setting-frame_limiter_provider option[value="proton"]')).toHaveCount(
    0,
  );
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ frame_limiter_provider: 'auto' });
});

test('legacy saved FEC zero preserves unrelated saves and can be corrected explicitly', async ({
  page,
}) => {
  const patches = await host(page, 'linux', { fec_percentage: 0, pyrowave: true });
  await page.goto('/v2/settings');
  await page.locator('#setting-pyrowave').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ pyrowave: false });
  await page.locator('#settings-group-everyday-everyday_encoding summary').click();
  await expect(page.getByText(/The saved zero is ignored by the host/)).toBeVisible();
  await page.locator('#setting-fec_percentage').fill('1');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(2);
  expect(patches[1]).toEqual({ fec_percentage: 1 });
  await page.locator('#setting-fec_percentage').fill('0');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect
    .poll(() =>
      page
        .locator('#setting-fec_percentage')
        .evaluate((input: HTMLInputElement) => input.validity.rangeUnderflow),
    )
    .toBe(true);
  expect(patches).toHaveLength(2);
});

test('standard FEC rejects zero while PyroWave critical FEC saves zero', async ({ page }) => {
  const patches = await host(page, 'linux', { pyrowave_critical_fec_percentage: 20 });
  await page.goto('/v2/settings');
  await page.locator('#settings-group-everyday-everyday_encoding summary').click();
  await page.locator('#setting-fec_percentage').fill('0');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect
    .poll(() =>
      page
        .locator('#setting-fec_percentage')
        .evaluate((input: HTMLInputElement) => input.validity.rangeUnderflow),
    )
    .toBe(true);
  expect(patches).toHaveLength(0);
  await page.locator('#setting-fec_percentage').fill('1');
  await page.getByRole('button', { name: 'Video & quality', exact: true }).click();
  await expect(page.locator('#setting-fec_percentage')).toHaveAttribute('min', '1');
  await page.locator('#setting-pyrowave_critical_fec_percentage').fill('0');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ fec_percentage: 1, pyrowave_critical_fec_percentage: 0 });
});

test('settings deep links open advanced encoders and back navigation preserves drafts', async ({
  page,
}) => {
  await host(page, 'linux', { encoder: 'vaapi' });
  await page.goto('/v2/settings?category=video#setting-vaapi_strict_rc_buffer');
  await expect(page.locator('#setting-vaapi_strict_rc_buffer')).toBeVisible();
  await page.locator('#setting-vaapi_strict_rc_buffer').check();
  await page.getByRole('button', { name: 'Everyday setup', exact: true }).click();
  await page.goBack();
  await expect(page.locator('#setting-vaapi_strict_rc_buffer')).toBeChecked();
});

test('Windows keeps automatic smoothness and platform-specific controls', async ({ page }) => {
  await host(page, 'windows');
  await page.goto('/v2/settings');
  await expect(page.locator('#setting-frame_limiter_auto_virtual_framegen')).toBeVisible();
  await expect(page.getByText('Display readiness')).toHaveCount(0);
  await page.goto('/v2/settings?category=display');
  await expect(page.locator('#setting-dd_use_sunshine_virtual_display_driver')).toBeVisible();
});

test('unavailable virtual screens explain repair and keep physical selection available', async ({
  page,
}) => {
  await host(page, 'linux', {}, false);
  await page.goto('/v2/settings');
  await expect(page.getByText('Setup needs attention', { exact: true })).toBeVisible();
  await expect(
    page.getByRole('link', { name: 'Open Linux setup and troubleshooting' }),
  ).toBeVisible();
  await page.locator('#setting-virtual_display_mode').selectOption('disabled');
  await expect(page.locator('#setting-output_name')).toBeVisible();
});

test('HTTP save rejection retains the draft', async ({ page }) => {
  await host(page);
  await localRoute(page, '**/api/config', async (route) => {
    if (route.request().method() === 'PATCH') await route.fulfill({ json: { status: false } });
    else await route.fallback();
  });
  await page.goto('/v2/settings');
  await page.locator('#setting-pyrowave').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.locator('#setting-pyrowave')).not.toBeChecked();
  await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toBeEnabled();
});

for (const width of [390, 768, 1100, 1440]) {
  test(`settings and save bar fit at ${width}px`, async ({ page }) => {
    await host(page);
    await page.setViewportSize({ width, height: 1000 });
    await page.goto('/v2/settings');
    await expect(page.locator('#setting-pyrowave')).toBeVisible();
    await page.locator('#setting-pyrowave').uncheck();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await expect(page.locator('.save-bar')).toBeVisible();
    const save = await page.locator('.save-bar').boundingBox();
    expect(save!.x).toBeGreaterThanOrEqual(0);
    expect(save!.x + save!.width).toBeLessThanOrEqual(width);
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.screenshot({
      path: join(screenshotDirectory, `everyday-${width}.png`),
      fullPage: true,
    });
  });
}

test('bulk unpair requires a confirmation and calls the existing endpoint', async ({ page }) => {
  await host(page);
  let calls = 0;
  await localRoute(page, '**/api/clients/unpair-all', async (route) => {
    calls++;
    await route.fulfill({ json: { status: true } });
  });
  await page.goto('/v2/devices');
  await page.getByRole('button', { name: 'Unpair all devices' }).click();
  expect(calls).toBe(0);
  await page.getByRole('dialog').getByRole('button', { name: 'Unpair all devices' }).click();
  await expect.poll(() => calls).toBe(1);
});

test('Windows maintenance uses server-directed download markup', async ({ page }) => {
  await host(page, 'windows');
  await page.goto('/v2/maintenance');
  const link = page.getByRole('link', { name: 'Download logs bundle' });
  await expect(link).toBeVisible();
  await expect(link).toHaveAttribute('href', '/api/logs/export');
  // Structural guard only; the download cases below exercise response behavior.
  // Match Logs navigation: do not supply a client-side download filename hint.
  await expect(link).not.toHaveAttribute('download');
});

for (const entryPoint of ['maintenance', 'logs']) {
  test(`Windows ${entryPoint} downloads the server-named log bundle`, async ({ page, context }) => {
    await host(page, 'windows');
    // A valid empty ZIP, served with the Windows endpoint's response headers.
    const archive = Buffer.from('504b0506000000000000000000000000000000000000', 'hex');
    const filename = 'vibepollo_logs-20261022-123456.zip';
    const requests: { method: string; cookie: string | null }[] = [];
    await context.addCookies([
      { name: 'session', value: 'log-bundle-test', url: 'http://127.0.0.1:5173' },
    ]);
    await localRoute(page, '**/api/logs/export', async (route) => {
      requests.push({
        method: route.request().method(),
        cookie: await route.request().headerValue('cookie'),
      });
      await route.fulfill({
        contentType: 'application/zip',
        headers: { 'Content-Disposition': `attachment; filename="${filename}"` },
        body: archive,
      });
    });
    await page.goto(`/v2/${entryPoint}`);
    const downloadStarted = page.waitForEvent('download');
    await page
      .getByRole(entryPoint === 'maintenance' ? 'link' : 'button', { name: 'Download logs bundle' })
      .click();
    const download = await downloadStarted;
    expect(download.suggestedFilename()).toBe(filename);
    expect(await download.failure()).toBeNull();
    expect(await readFile((await download.path())!)).toEqual(archive);
    expect(requests).toEqual([{ method: 'GET', cookie: 'session=log-bundle-test' }]);
    await expect(page).toHaveURL(`/v2/${entryPoint}`);
  });
}

test('Linux maintenance offers logs and display setup', async ({ page }) => {
  await host(page);
  await page.goto('/v2/maintenance');
  await expect(page.getByRole('link', { name: 'Open and download logs' })).toBeVisible();
  await expect(page.getByRole('link', { name: 'Display settings' })).toBeVisible();
  await expect(page.getByText('Updates and release notes', { exact: true })).toBeVisible();
});

for (const theme of ['dark', 'light']) {
  test(`${theme} theme and keyboard switches remain usable`, async ({ page }) => {
    await host(page);
    await page.addInitScript((theme) => localStorage.setItem('vibepollo.theme', theme), theme);
    await page.goto('/v2/settings');
    await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
    await page.locator('#setting-pyrowave').focus();
    await page.keyboard.press('Space');
    await expect(page.locator('#setting-pyrowave')).not.toBeChecked();
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.screenshot({
      path: join(screenshotDirectory, `everyday-${theme}.png`),
      fullPage: true,
    });
  });
}

test('all canonical v2 pages render without client-side exceptions', async ({ page }) => {
  await host(page);
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  for (const path of [
    '',
    'library',
    'library/new',
    'devices',
    'pair',
    'stats',
    'integrations',
    'logs',
    'api-tokens',
    'stream',
    'maintenance',
  ]) {
    await page.goto(`/v2/${path}`);
    await expect(page.locator('main')).toBeVisible();
    await expect(page.locator('main h1')).toBeVisible();
  }
  expect(errors).toEqual([]);
});

test('Linux integration metadata exposes Steam controls', async ({ page }) => {
  await host(page);
  await page.goto('/v2/integrations');
  await expect(page.locator('.integration-row--steam')).toBeVisible();
  await expect(page.locator('#steam-auto-sync')).toBeVisible();
});

test('search reaches integration settings and Linux adapters accept a render-device path', async ({
  page,
}) => {
  const patches = await host(page);
  await page.goto('/v2/settings?category=display#setting-adapter_name');
  await page.locator('#setting-adapter_name').fill('/dev/dri/renderD129');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ adapter_name: '/dev/dri/renderD129' });
  await page.getByRole('searchbox', { name: 'Search settings' }).fill('lutris');
  await expect(page.locator('.settings-destinations a')).toHaveAttribute(
    'href',
    '/v2/integrations#integration-lutris',
  );
});

test('Windows Playnite policies load and save inside v2', async ({ page }) => {
  const patches = await host(page, 'windows');
  await page.goto('/v2/integrations#playnite-policies');
  await expect(page.locator('#playnite_auto_sync')).toBeVisible();
  await page.locator('#playnite_auto_sync').uncheck();
  await page
    .locator('#playnite-policies')
    .getByRole('button', { name: 'Save changes', exact: true })
    .click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ playnite_auto_sync: false });
});

test('mobile navigation traps focus and returns it to the menu button', async ({ page }) => {
  await host(page);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto('/v2/settings');
  const menu = page.getByRole('button', { name: 'Open navigation' });
  await menu.click();
  await expect(page.locator('#app-navigation')).toHaveAttribute('aria-modal', 'true');
  await page.keyboard.press('Escape');
  await expect(menu).toBeFocused();
});

test('failed configuration load keeps editing unavailable until retry succeeds', async ({
  page,
}) => {
  await host(page);
  let failed = true;
  await localRoute(page, '**/api/config', async (route) => {
    if (failed) await route.fulfill({ status: 503, json: { status: false } });
    else await route.fallback();
  });
  await page.goto('/v2/settings');
  await expect(page.getByRole('alert')).toBeVisible();
  await expect(page.locator('#setting-pyrowave')).toHaveCount(0);
  failed = false;
  await page.getByRole('button', { name: 'Reload', exact: true }).click();
  await expect(page.locator('#setting-pyrowave')).toBeVisible();
});

test('edits made during a settings save remain unsaved', async ({ page }) => {
  await host(page);
  let finish!: () => void;
  const pending = new Promise<void>((resolve) => {
    finish = resolve;
  });
  let started = false;
  await localRoute(page, '**/api/config', async (route) => {
    if (route.request().method() !== 'PATCH') {
      await route.fallback();
      return;
    }
    started = true;
    await pending;
    await route.fulfill({ json: { status: true } });
  });
  await page.goto('/v2/settings');
  await page.locator('#setting-pyrowave').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => started).toBe(true);
  await page.locator('#setting-wayland_hdr_compatibility').check();
  finish();
  await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toBeEnabled();
  await expect(page.locator('#setting-wayland_hdr_compatibility')).toBeChecked();
  await expect(page.locator('.save-bar')).toContainText('1 unsaved change');
});

test('physical screen controls fit with long output names at intermediate widths', async ({
  page,
}) => {
  await host(page, 'linux', {
    virtual_display_mode: 'disabled',
    output_name:
      'An unusually long display name with a persistent device identifier - ' + 'a'.repeat(100),
  });
  await page.setViewportSize({ width: 1100, height: 900 });
  await page.goto('/v2/settings');
  await expect(page.locator('#setting-output_name')).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  const row = page.locator('#setting-output_name');
  const bounds = await row.boundingBox();
  expect(bounds!.x + bounds!.width).toBeLessThanOrEqual(1100);
});

test('settings reflow at a 200% zoom-equivalent viewport with forced colors and reduced motion', async ({
  page,
}) => {
  await host(page);
  await page.emulateMedia({ forcedColors: 'active', reducedMotion: 'reduce' });
  // Browser zoom halves the CSS viewport; CSS zoom alone does not update media queries.
  await page.setViewportSize({ width: 640, height: 450 });
  await page.goto('/v2/settings');
  await page.locator('#setting-pyrowave').focus();
  await page.keyboard.press('Space');
  await expect(page.locator('#setting-pyrowave')).not.toBeChecked();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.locator('.save-bar')).toHaveCount(0);
});

test('MangoHud saves preserve later edits and report rejected requests', async ({ page }) => {
  await host(page);
  await localRoute(page, '**/api/frame-limiter/status', async (route) => {
    await route.fulfill({
      json: {
        enabled: true,
        configured_provider: 'mangohud',
        fps_limit: 0,
        mangohud_available: true,
      },
    });
  });
  let finish!: () => void;
  const pending = new Promise<void>((resolve) => {
    finish = resolve;
  });
  let calls = 0;
  await localRoute(page, '**/api/config', async (route) => {
    if (route.request().method() !== 'PATCH') {
      await route.fallback();
      return;
    }
    const call = ++calls;
    if (call === 1) await pending;
    await route.fulfill({ json: { status: call === 1 } });
  });
  await page.goto('/v2/integrations#integration-mangohud');
  await page.locator('#mangohud-fps-limit').fill('90');
  const section = page.locator('section[aria-labelledby="mangohud-settings-heading"]');
  await section.getByRole('button', { name: 'Apply', exact: true }).click();
  await expect.poll(() => calls).toBe(1);
  await page.locator('#mangohud-fps-limit').fill('120');
  finish();
  await expect(section.getByRole('button', { name: 'Apply', exact: true })).toBeEnabled();
  await expect(page.locator('#mangohud-fps-limit')).toHaveValue('120');
  await section.getByRole('button', { name: 'Apply', exact: true }).click();
  await expect.poll(() => calls).toBe(2);
  await expect(section.getByRole('button', { name: 'Apply', exact: true })).toBeEnabled();
  await expect(page.locator('#mangohud-fps-limit')).toHaveValue('120');
});

test('mobile navigation keeps hidden controls out of the tab order and releases the page on resize', async ({
  page,
}) => {
  await host(page);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto('/v2/');
  const menu = page.getByRole('button', { name: 'Open navigation' });
  const navigation = page.locator('#app-navigation');
  await menu.focus();
  await page.keyboard.press('Tab');
  await page.keyboard.press('Tab');
  await expect(page.getByRole('button', { name: 'Refresh', exact: true })).toBeFocused();
  await menu.click();
  const brand = navigation.getByRole('link', { name: 'Vibepollo overview' });
  await expect(brand).toBeFocused();
  await page.keyboard.press('Shift+Tab');
  await expect(navigation.getByRole('button', { name: 'Logout' })).toBeFocused();
  await page.keyboard.press('Tab');
  await expect(brand).toBeFocused();
  await expect(page.locator('body')).toHaveCSS('overflow', 'hidden');
  await page.setViewportSize({ width: 1100, height: 844 });
  await expect(navigation).not.toHaveAttribute('aria-modal', 'true');
  await expect(page.locator('main')).not.toHaveAttribute('inert', '');
  await expect(page.locator('body')).not.toHaveCSS('overflow', 'hidden');
});

test('appearance controls persist the chosen theme and follow system appearance', async ({
  page,
}) => {
  await host(page);
  await page.emulateMedia({ colorScheme: 'dark' });
  await page.goto('/v2/');
  const appearance = page.getByRole('group', { name: 'Appearance' });
  await appearance.getByRole('button', { name: 'Light', exact: true }).click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await page.reload();
  await expect(appearance.getByRole('button', { name: 'Light', exact: true })).toHaveAttribute(
    'aria-pressed',
    'true',
  );
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
  await appearance.getByRole('button', { name: 'System', exact: true }).click();
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'dark');
  await page.emulateMedia({ colorScheme: 'light' });
  await expect(page.locator('html')).toHaveAttribute('data-theme', 'light');
});

test('overview leads with readiness and keeps display guidance available on demand', async ({
  page,
}) => {
  await host(page);
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream' })).toBeVisible();
  await expect(page.getByRole('link', { name: 'Pair a device', exact: true })).toHaveAttribute(
    'href',
    '/v2/pair',
  );
  await expect(page.locator('.linux-capture')).not.toHaveAttribute('open', '');
  await page.locator('.linux-capture > summary').click();
  await expect(page.locator('.linux-capture__body')).toBeVisible();
  await page.getByRole('link', { name: 'Pair a device', exact: true }).click();
  await expect(page.locator('.nav-link[aria-current="page"]')).toHaveText('Devices');
});

test('overview shows repair guidance when display setup is unavailable', async ({ page }) => {
  await host(page, 'linux', {}, false);
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Needs attention', exact: true })).toBeVisible();
  await expect(page.getByRole('link', { name: 'Review setup', exact: true })).toHaveAttribute(
    'href',
    '/v2/settings?category=display',
  );
  await expect(
    page.getByRole('link', { name: 'Open Linux setup and troubleshooting' }),
  ).toBeVisible();
});

test('unknown readiness and unavailable utilization do not render as successful measurements', async ({
  page,
}) => {
  await host(page);
  await localRoute(page, '**/api/metadata', (route) => route.fulfill({ json: { platform: 'linux' } }));
  await localRoute(page, '**/api/host/stats', (route) =>
    route.fulfill({ json: { cpu_percent: -1, gpu_percent: null } }),
  );
  await page.goto('/v2/');
  await expect(page.locator('.readiness-panel')).toHaveAttribute('data-tone', 'neutral');
  await expect(page.locator('.metric-grid dd')).toHaveText([
    'Unavailable',
    'Unavailable',
    'Unavailable',
    'Unavailable',
  ]);
  await expect(page.locator('.metric-footnote')).toHaveCount(0);
  await expect(page.getByRole('link', { name: 'Review setup', exact: true })).toBeVisible();
});

async function libraryWithMissingCovers(page: Page) {
  await localRoute(page, '**/api/apps', (route) =>
    route.fulfill({
      json: {
        apps: [
          { uuid: 'one', name: 'Elden Ring', 'steam-id': '1245620', 'steam-managed': 'auto' },
          { uuid: 'two', name: 'Dolphin Emulator', cmd: '/usr/bin/dolphin-emu' },
          {
            uuid: 'three',
            name: 'An application with a very long localized name that should fit the available space',
            cmd: '/some/long/path/to/a/game',
          },
        ],
      },
    }),
  );
  await localRoute(page, '**/api/apps/*/cover', (route) => route.fulfill({ status: 404, body: '' }));
}

for (const available of [false, true]) {
  test(`application cover explains Moonlight availability (${available})`, async ({
    page,
  }, testInfo) => {
    await host(page, 'windows');
    await page.route('**/api/apps', (route) =>
      route.fulfill({
        json: {
          apps: [
            { uuid: '11111111-2222-4333-8444-555555555555', name: 'Custom desktop', 'image-path': 'C:/covers/custom.png' },
          ],
        },
      }),
    );
    await page.route('**/api/apps/11111111-2222-4333-8444-555555555555/cover*', (route) =>
      available
        ? route.fulfill({
            contentType: 'image/png',
            body: Buffer.from(
              'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQI12P4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==',
              'base64',
            ),
          })
        : route.fulfill({ status: 404, json: { error: 'Configured cover unavailable' } }),
    );
    await page.goto('/v2/library/11111111-2222-4333-8444-555555555555');
    await expect(page.locator('#app-image-path')).toHaveValue('C:/covers/custom.png');
    await expect(
      page.getByText(/Custom Moonlight covers need a readable PNG file no larger than 16 MiB/),
    ).toBeVisible();
    if (available) {
      await expect(page.locator('.editor-artwork img')).toBeVisible();
      await expect(page.getByText('Configured cover unavailable', { exact: true })).toHaveCount(0);
    } else {
      await expect(page.locator('.editor-artwork img')).toHaveCount(0);
      await expect(page.getByText('Configured cover unavailable', { exact: true })).toBeVisible();
    }
    await page.screenshot({ path: testInfo.outputPath('cover.png'), fullPage: true });
  });
}

test('library placeholders preserve search, keyboard selection, and list preferences', async ({
  page,
}) => {
  await host(page);
  await libraryWithMissingCovers(page);
  await page.goto('/v2/library');
  await expect(page.locator('.library-item__artwork-fallback')).toHaveCount(3);
  await expect(
    page.locator('.library-item__source').filter({ hasText: 'Steam managed' }),
  ).toBeVisible();
  await page.getByRole('searchbox', { name: 'Search applications' }).fill('dolphin');
  await expect(page.locator('[data-library-item]')).toHaveCount(1);
  await page.getByRole('option', { name: 'Dolphin Emulator' }).focus();
  await page.keyboard.press('Space');
  await expect(page.locator('.library-selection .vs-status-badge')).toContainText('1 selected');
  await page.keyboard.press('Escape');
  await expect(page.locator('.library-selection')).toHaveCount(0);
  await page.getByRole('button', { name: 'List', exact: true }).click();
  await expect(page).toHaveURL(/[?&]q=dolphin(?:&|$)/);
  await page.reload();
  await expect(page.locator('.library-collection')).toHaveClass(/library-collection--list/);
  await expect(page.getByRole('searchbox', { name: 'Search applications' })).toHaveValue('dolphin');
});

for (const width of [320, 390, 768, 1100, 1440]) {
  test(`main workflows reflow without horizontal scrolling at ${width}px`, async ({
    page,
  }, testInfo) => {
    await host(page);
    await libraryWithMissingCovers(page);
    await page.setViewportSize({ width, height: 900 });
    for (const path of ['', 'library', 'library/new', 'devices']) {
      await page.goto(`/v2/${path}`);
      await expect(page.locator('main h1')).toBeVisible();
      await expect(page.locator('.vs-loading-skeleton')).toHaveCount(0);
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
        true,
      );
      await page.screenshot({
        path: testInfo.outputPath(`${(path || 'overview').replaceAll('/', '-')}-${width}.png`),
        fullPage: true,
      });
    }
  });
}

test('a failed performance refresh retains the sample and visibly marks it as stale', async ({
  page,
}) => {
  await host(page);
  let unavailable = false;
  await localRoute(page, '**/api/host/stats', (route) =>
    unavailable
      ? route.fulfill({ status: 503, json: { status: false } })
      : route.fulfill({
          json: { cpu_percent: 12, gpu_percent: 8, ram_percent: 24, vram_percent: 11 },
        }),
  );
  await page.goto('/v2/');
  await expect(page.locator('.metric-grid dd').first()).toHaveText('12%');
  unavailable = true;
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await expect(
    page.getByText('Showing the last available sample. Refresh to try again.'),
  ).toBeVisible();
  await expect(page.locator('.metric-grid dd').first()).toHaveText('12%');
  unavailable = false;
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await expect(page.locator('.overview-stale')).toHaveCount(0);
});

for (const platform of ['linux', 'windows']) {
  test(`application picker respects ${platform} integration availability`, async ({ page }) => {
    await host(page, platform);
    const requests: string[] = [];
    page.on('request', (request) => {
      requests.push(new URL(request.url()).pathname);
    });
    await page.goto('/v2/library/new');
    await page.locator('#app-name').fill('A new game');
    await expect(page.getByRole('button', { name: 'Browse games', exact: true })).toBeVisible();
    if (platform === 'linux') {
      expect(requests.some((path) => path.startsWith('/api/playnite/'))).toBe(false);
    } else {
      await expect.poll(() => requests.includes('/api/playnite/status')).toBe(true);
      expect(requests.some((path) => path.startsWith('/api/lutris/'))).toBe(false);
    }
  });
}

for (const platform of ['windows', 'linux']) {
  test(`game search groups sources and fills the selected ${platform} library`, async ({
    page,
  }) => {
    await host(page, platform, {}, true, { steam: true, lutris: platform === 'linux' });
    const saved: Record<string, unknown>[] = [];
    await localRoute(page, '**/api/apps', (route) => {
      if (route.request().method() === 'POST') {
        saved.push(route.request().postDataJSON());
        return route.fulfill({
          status: 400,
          json: { status: false, error: 'Keep editor open for assertions' },
        });
      }
      return route.fulfill({ json: { apps: [] } });
    });
    await localRoute(page, '**/api/playnite/status', (route) =>
      route.fulfill({ json: { installed: true, active: true } }),
    );
    await localRoute(page, '**/api/playnite/games', (route) =>
      route.fulfill({ json: [{ id: 'playnite-portal', name: 'Portal', installed: true }] }),
    );
    await localRoute(page, '**/api/steam/games*', (route) =>
      route.fulfill({
        json: {
          games: [
            {
              appid: 400,
              name: 'Portal',
              installed: true,
              install_dir: 'C:/Steam/Portal',
              artwork_client_path: 'C:/covers/steam_400.png',
            },
            {
              appid: 620,
              name: 'Portal 2',
              installed: true,
              install_dir: 'C:/Steam/Portal2',
              artwork_client_path: 'C:/covers/steam_620.png',
            },
          ],
        },
      }),
    );
    await localRoute(page, '**/api/lutris/games', (route) =>
      route.fulfill({
        json: {
          games: [
            {
              id: '9',
              name: 'Portal',
              runner: 'wine',
              directory: '/games/portal',
              image_path: '/covers/lutris_9.png',
            },
          ],
        },
      }),
    );
    await page.goto('/v2/library/new');
    await page.locator('#app-name').fill('Portal');
    const result = page.getByRole('option', {
      name: platform === 'windows' ? 'Portal Playnite · Steam' : 'Portal Steam · Lutris',
      exact: true,
    });
    await expect(result).toBeVisible();
    await result.click();
    const source = page.getByRole('combobox', { name: 'Launch with' });
    await expect(source).toHaveValue(
      platform === 'windows' ? 'playnite:playnite-portal' : 'steam:400',
    );
    await source.selectOption(platform === 'windows' ? 'steam:400' : 'lutris:9');
    await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
    await expect.poll(() => saved.length).toBe(1);
    expect(saved[0]).toMatchObject(
      platform === 'windows'
        ? {
            'steam-id': '400',
            cmd: 'cmd /c start "" steam://rungameid/400',
            'image-path': 'C:/covers/steam_400.png',
          }
        : {
            'lutris-id': '9',
            cmd: 'lutris lutris:rungameid/9',
            'image-path': '/covers/lutris_9.png',
          },
    );
    expect(saved[0]).not.toHaveProperty('playnite-id');
    await page.locator('#app-name').fill('Portal 2');
    await page.locator('#app-name').press('ArrowDown');
    await page.locator('#app-name').press('Enter');
    await expect(source).toHaveCount(0);
    await expect(page.locator('#app-name')).toHaveValue('Portal 2');
    await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
    await expect.poll(() => saved.length).toBe(2);
    expect(saved[1]).toMatchObject({ 'steam-id': '620', 'image-path': 'C:/covers/steam_620.png' });
    expect(saved[1]).not.toHaveProperty('lutris-id');
  });
}

test('game library setup supports multiple Windows libraries without enabling Steam by default', async ({
  page,
}, testInfo) => {
  const patches = await host(page, 'windows', {}, true, { steam: true, playnite_toggle: true });
  await localRoute(page, '**/api/playnite/status', (route) =>
    route.fulfill({ json: { installed: true, active: true } }),
  );
  await page.goto('/v2/library');
  await page.getByRole('button', { name: 'Library manager settings', exact: true }).click();
  const dialog = page.getByRole('dialog', {
    name: /Setup Game Library Integration|Library manager settings/,
  });
  await expect(dialog.getByText('Recommended on Windows')).toBeVisible();
  await expect(dialog.getByRole('checkbox', { name: 'Playnite', exact: true })).toBeChecked();
  await expect(dialog.getByRole('checkbox', { name: 'Steam', exact: true })).not.toBeChecked();
  await expect(dialog.getByRole('checkbox', { name: 'Lutris', exact: true })).toHaveCount(0);
  await dialog.getByRole('checkbox', { name: 'Steam', exact: true }).check();
  await dialog.getByRole('button', { name: 'Next: Library settings' }).click();
  await dialog.getByRole('spinbutton', { name: 'Recent games', exact: true }).last().fill('7');
  await dialog.getByRole('button', { name: 'Save settings', exact: true }).click();
  await expect(dialog.getByRole('status')).toHaveText('Game library settings saved.');
  expect(patches.at(-1)).toMatchObject({
    playnite_enabled: true,
    playnite_auto_sync: true,
    steam_enabled: true,
    steam_auto_sync: true,
    steam_sync_all_installed: false,
    steam_recent_games: 7,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await dialog.evaluate((element) => element.scrollWidth <= element.clientWidth)).toBe(true);
  await page.screenshot({ path: testInfo.outputPath('library-setup-mobile.png') });
});

test('saving a Steam selection waits for its prepared cover', async ({ page }) => {
  await host(page, 'windows', {}, true, { steam: true });
  const saved: Record<string, unknown>[] = [];
  await localRoute(page, '**/api/steam/games*', async (route) => {
    const selected = new URL(route.request().url()).searchParams.has('appid');
    if (selected) await new Promise((resolve) => setTimeout(resolve, 350));
    await route.fulfill({
      json: {
        games: [
          {
            appid: 400,
            name: 'Portal',
            installed: true,
            ...(selected ? { artwork_client_path: 'C:/covers/steam_400.png' } : {}),
          },
        ],
      },
    });
  });
  await localRoute(page, '**/api/apps', (route) => {
    if (route.request().method() === 'POST') saved.push(route.request().postDataJSON());
    return route.fulfill({
      json: route.request().method() === 'POST' ? { status: true } : { apps: [] },
    });
  });
  await page.goto('/v2/library/new');
  await page.locator('#app-name').fill('Portal');
  await page.getByRole('option', { name: 'Portal Steam', exact: true }).click();
  await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
  await expect.poll(() => saved.length).toBe(1);
  expect(saved[0]).toMatchObject({
    'steam-id': '400',
    'image-path': 'C:/covers/steam_400.png',
    'steam-artwork-client-compatible': true,
  });
});

test('encoder failures direct the overview to diagnostics', async ({ page }) => {
  await host(page, 'windows');
  await localRoute(page, '**/api/metadata', (route) =>
    route.fulfill({
      json: {
        platform: 'windows',
        encoder_status: { state: 'failed', h264: false },
      },
    }),
  );
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Needs attention', exact: true })).toBeVisible();
  await expect(page.locator('.readiness-panel .button--primary')).toHaveAttribute(
    'href',
    '/v2/logs',
  );
});

test('unavailable Linux providers are neither offered nor called', async ({ page }) => {
  await host(page, 'linux', {}, true, {});
  const providerRequests: string[] = [];
  page.on('request', (request) => {
    if (/\/api\/(steam|lutris|frame-limiter)\//.test(request.url()))
      providerRequests.push(request.url());
  });
  await page.goto('/v2/integrations');
  await expect(
    page.locator('#integration-steam, #integration-lutris, #integration-mangohud'),
  ).toHaveCount(0);
  await page.getByRole('button', { name: 'Setup Game Library Integration', exact: true }).click();
  const setup = page.getByRole('dialog');
  await expect(setup.getByRole('checkbox')).toHaveCount(0);
  await setup.getByRole('button', { name: 'Cancel', exact: true }).click();
  expect(providerRequests).toEqual([]);
});

for (const width of [390, 1440]) {
  test(`app behavior edits preserve legacy values and unknown fields at ${width}px`, async ({
    page,
  }) => {
    await host(page, 'windows');
    await page.setViewportSize({ width, height: 1000 });
    const uuid = '12345678-1234-4234-8234-123456789abc';
    const original = {
      uuid,
      name: 'Compatibility test',
      cmd: '',
      'allow-client-commands': false,
      'use-app-identity': 'false',
      'per-client-app-identity': true,
      'scale-factor': '125',
      gamepad: 'legacy-custom-controller',
      'state-cmd': [{ do: 'start', undo: 'stop', elevated: false, custom: { retained: true } }],
      'custom-field': { retained: ['value'] },
      'config-overrides': { rtx_hdr: 'disabled' },
    };
    let saved: Record<string, unknown> | undefined;
    await localRoute(page, '**/api/apps', async (route) => {
      if (route.request().method() === 'POST') {
        saved = route.request().postDataJSON();
        await route.fulfill({ json: { status: true } });
      } else await route.fulfill({ json: { apps: [original] } });
    });
    await page.goto(`/v2/library/${uuid}`);
    await expect(page.locator('#app-allow-client-commands')).toHaveValue('false');
    await expect(page.locator('#app-use-app-identity')).toHaveValue('false');
    await expect(page.locator('#app-gamepad')).toHaveValue('legacy-custom-controller');
    await expect(page.locator('#app-scale-factor')).toHaveValue('125');
    await expect(page.locator('#app-terminate-on-pause')).toHaveValue('');
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await page
      .locator('.app-compatibility')
      .screenshot({ path: join(screenshotDirectory, `app-behavior-${width}.png`) });
    await page.locator('#app-state-do-0').fill('new start');
    await page.locator('#app-terminate-on-pause').selectOption('true');
    await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
    await expect.poll(() => saved?.['terminate-on-pause']).toBe(true);
    expect(saved?.['allow-client-commands']).toBe(false);
    expect(saved?.['use-app-identity']).toBe('false');
    expect(saved?.['per-client-app-identity']).toBe(true);
    expect(saved?.['scale-factor']).toBe('125');
    expect(saved?.gamepad).toBe('legacy-custom-controller');
    expect(saved?.['exclude-global-state-cmd']).toBeUndefined();
    expect(saved?.['custom-field']).toEqual(original['custom-field']);
    expect(saved?.['state-cmd']).toEqual([{ ...original['state-cmd'][0], do: 'new start' }]);
  });
}

test('app behavior validates edited scaling and can return explicit options to defaults', async ({
  page,
}) => {
  await host(page, 'windows');
  const uuid = '12345678-1234-4234-8234-123456789abc';
  let saved: Record<string, unknown> | undefined;
  await localRoute(page, '**/api/apps', async (route) => {
    if (route.request().method() === 'POST') {
      saved = route.request().postDataJSON();
      await route.fulfill({ json: { status: true } });
    } else
      await route.fulfill({
        json: { apps: [{ uuid, name: 'Defaults', cmd: '', 'allow-client-commands': false }] },
      });
  });
  await page.goto(`/v2/library/${uuid}`);
  await page.locator('#app-allow-client-commands').selectOption('');
  await page.locator('#app-scale-factor').fill('0');
  await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
  expect(saved).toBeUndefined();
  expect(
    await page
      .locator('#app-scale-factor')
      .evaluate((input: HTMLInputElement) => input.validity.rangeUnderflow),
  ).toBe(true);
  await page.locator('#app-scale-factor').fill('150');
  await page.getByRole('button', { name: 'Save application', exact: true }).first().click();
  await expect.poll(() => saved?.['scale-factor']).toBe(150);
  expect(saved?.['allow-client-commands']).toBeUndefined();
  expect(saved?.['state-cmd']).toBeUndefined();
  expect(saved?.['use-app-identity']).toBeUndefined();
});

for (const platform of ['windows', 'linux']) {
  test(`classic game library setup and search work on ${platform}`, async ({ page }) => {
    const legacyUrl = process.env.VIBEPOLLO_LEGACY_TEST_URL;
    test.skip(!legacyUrl, 'Set VIBEPOLLO_LEGACY_TEST_URL to a served classic UI build.');
    const patches = await host(page, platform, {}, true, { steam: true, lutris: platform === 'linux' });
    await localRoute(page, '**/api/playnite/status', (route) =>
      route.fulfill({ json: { installed: true, active: true } }),
    );
    await localRoute(page, '**/api/playnite/games', (route) =>
      route.fulfill({ json: [{ id: 'pn-portal', name: 'Portal', installed: true }] }),
    );
    await localRoute(page, '**/api/steam/games*', (route) =>
      route.fulfill({
        json: {
          games: [
            {
              appid: 400,
              name: 'Portal',
              installed: true,
              artwork_client_path: 'C:/covers/steam_400.png',
            },
          ],
        },
      }),
    );
    await localRoute(page, '**/api/lutris/games', (route) =>
      route.fulfill({ json: { games: [{ id: 9, name: 'Portal', directory: '/games/portal' }] } }),
    );
    const saved: Record<string, unknown>[] = [];
    await localRoute(page, '**/api/apps', (route) => {
      if (route.request().method() === 'POST') saved.push(route.request().postDataJSON());
      return route.fulfill({
        json:
          route.request().method() === 'POST'
            ? { status: true }
            : {
                apps: saved.map((app) => ({
                  ...app,
                  uuid: '53544541-4d00-5000-8000-000000000190',
                })),
              },
      });
    });
    await localRoute(page, '**/api/apps/*/cover*', (route) =>
      route.fulfill({
        contentType: 'image/png',
        body: Buffer.from(
          'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVQI12P4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==',
          'base64',
        ),
      }),
    );
    await page.goto(legacyUrl + '/applications');
    await page
      .getByRole('button', { name: /Setup Game Library Integration|Library manager settings/ })
      .first()
      .click();
    const setup = page.getByRole('dialog', {
      name: /Setup Game Library Integration|Library manager settings/,
    });
    await expect(setup.getByRole('checkbox', { name: 'Steam', exact: true })).not.toBeChecked();
    await setup.getByRole('checkbox', { name: 'Steam', exact: true }).check();
    await setup.getByRole('button', { name: 'Next: Library settings' }).click();
    await setup.getByRole('button', { name: 'Save settings', exact: true }).click();
    await expect(setup.getByRole('status')).toHaveText('Game library settings saved.');
    expect(patches.at(-1)).toMatchObject({
      steam_auto_sync: true,
      [platform === 'windows' ? 'playnite_auto_sync' : 'lutris_auto_sync']: true,
    });
    await setup.getByRole('button', { name: 'Done', exact: true }).click();
    await page
      .getByRole('button', { name: /Add Application/ })
      .first()
      .click();
    const editor = page.locator('.n-card.n-modal');
    const search = editor.locator('.n-select').first();
    await search.click();
    await page
      .getByText(platform === 'windows' ? 'Portal — Playnite · Steam' : 'Portal — Steam · Lutris', {
        exact: true,
      })
      .click();
    await expect(editor.getByText('Launch with', { exact: true })).toBeVisible();
    await editor.locator('.n-select').nth(1).click();
    await page
      .locator('.n-base-select-option')
      .filter({ hasText: platform === 'windows' ? /^Steam$/ : /^Lutris$/ })
      .click();
    await editor.getByRole('button', { name: /Save/ }).click();
    await expect.poll(() => saved.length).toBe(1);
    expect(saved[0]).toMatchObject(
      platform === 'windows'
        ? {
            'steam-id': '400',
            cmd: 'cmd /c start "" steam://rungameid/400',
            'image-path': 'C:/covers/steam_400.png',
          }
        : { 'lutris-id': '9', cmd: 'lutris lutris:rungameid/9' },
    );
    expect(saved[0]).not.toHaveProperty('playnite-id');
    await expect(page.locator('.apps-row__badge')).toHaveText(
      platform === 'windows' ? 'Steam' : 'Lutris',
    );
    await expect(page.locator('.apps-row__icon')).toHaveAttribute('src', /\/cover/);
  });
}

test('initial library setup refreshes apps and persists editable settings', async ({ page }) => {
  await host(page, 'windows', {}, true, { steam: true });
  await localRoute(page, '**/api/playnite/status', route => route.fulfill({ json: { installed: false } }));
  let synced = false;
  await localRoute(page, '**/api/steam/force_sync', (route) => {
    synced = true;
    return route.fulfill({ json: { status: true } });
  });
  await localRoute(page, '**/api/apps', (route) =>
    route.fulfill({
      json: {
        apps: synced ? [{ name: 'Synced Steam game', uuid: 'steam-test', 'steam-id': '42' }] : [],
      },
    }),
  );
  await page.goto('/v2/library');
  await page.getByRole('button', { name: 'Setup Game Library Integration', exact: true }).click();
  const dialog = page.getByRole('dialog');
  await dialog.getByRole('checkbox', { name: 'Steam', exact: true }).check();
  await dialog.getByRole('button', { name: 'Next: Library settings' }).click();
  await dialog.getByRole('spinbutton', { name: 'Recent games', exact: true }).fill('6');
  await dialog.getByRole('button', { name: 'Save settings', exact: true }).click();
  await dialog.getByRole('button', { name: 'Done', exact: true }).click();
  await expect(page.getByText('Synced Steam game', { exact: true }).first()).toBeVisible();
  await expect(
    page.getByRole('button', { name: 'Setup Game Library Integration', exact: true }),
  ).toHaveCount(0);
  await page.reload();
  await page.getByRole('button', { name: 'Library manager settings', exact: true }).click();
  await dialog.getByRole('button', { name: 'Next: Library settings' }).click();
  await expect(dialog.getByRole('spinbutton', { name: 'Recent games', exact: true })).toHaveValue(
    '6',
  );
});

for (const platform of ['linux', 'windows'] as const) {
  test(`${platform} logs download requests the retained bundle even with an empty viewer`, async ({
    page,
  }) => {
    await host(page, platform, {});
    await localRoute(page, '**/api/logs?*', (route) =>
      route.fulfill({ body: '', contentType: 'text/plain' }),
    );
    await localRoute(page, '**/api/logs/export', (route) =>
      route.fulfill({
        body: 'test bundle',
        contentType: 'application/zip',
        headers: { 'Content-Disposition': 'attachment; filename="vibepollo_logs.zip"' },
      }),
    );
    await page.goto('/v2/logs');
    const download = page.waitForEvent('download');
    await page.getByRole('button', { name: 'Download logs bundle' }).click();
    expect((await download).suggestedFilename()).toBe('vibepollo_logs.zip');
  });
}

async function settleUpdaterRendering(page: Page) {
  await page.evaluate(() => new Promise<void>((resolve) =>
    requestAnimationFrame(() => requestAnimationFrame(() => resolve())),
  ));
}

test('Linux shows an automatic update notice across pages and retries failed checks', async ({
  page,
}) => {
  await host(page);
  let unavailable = true;
  await page.route('https://api.github.com/repos/Nonary/Vibepollo/releases?**', async (route) => {
    await route.fulfill(
      unavailable
        ? { status: 403, json: { message: 'API rate limit exceeded' } }
        : {
            json: [
              {
                tag_name: 'v1.1.0',
                draft: false,
                prerelease: false,
                assets: [{ name: 'Vibepollo-x86_64.AppImage', state: 'uploaded', size: 100 }],
                html_url: 'https://github.com/Nonary/Vibepollo/releases/tag/v1.1.0',
              },
            ],
          },
    );
  });
  await page.goto('/v2/');
  const notice = page.locator('.update-notice');
  await expect(
    notice.getByText('Release information is unavailable. Try again later.'),
  ).toBeVisible();
  unavailable = false;
  await notice.getByRole('button', { name: 'Check for updates' }).click();
  await expect(notice.getByText('Vibepollo 1.1.0 is available')).toBeVisible();
  await expect(notice.getByRole('link', { name: 'Read release notes' })).toHaveAttribute(
    'href',
    'https://github.com/Nonary/Vibepollo/releases/tag/v1.1.0',
  );
  await page.getByRole('link', { name: 'Library', exact: true }).click();
  await expect(notice.getByText('Vibepollo 1.1.0 is available')).toBeVisible();
});

test('Linux stable install respects an explicit prerelease opt-out', async ({ page }) => {
  await host(page, 'linux', { notify_pre_releases: false });
  await page.route('https://api.github.com/repos/Nonary/Vibepollo/releases?**', async (route) => {
    await route.fulfill({
      json: [{
        tag_name: 'v2.0.0-beta.1',
        draft: false,
        prerelease: true,
        assets: [{ name: 'Vibepollo-x86_64.AppImage', state: 'uploaded', size: 100 }],
      }],
    });
  });
  const response = page.waitForResponse((response) =>
    response.url().startsWith('https://api.github.com/repos/Nonary/Vibepollo/releases?'),
  );
  await page.goto('/v2/');
  await (await response).finished();
  await settleUpdaterRendering(page);
  await expect(page.locator('.update-notice')).toHaveCount(0);
});

test('Windows default nightly notification validates metadata and dismissal survives reload', async ({
  page,
}) => {
  await host(page, 'windows', {}, true, undefined, { version: '1.9.0' });
  const tag = 'daily-2026-10-11';
  const metadata = {
    schema_version: 1, channel: 'nightly', tag,
    version: '2.0.0-daily.d20261011+aaaaaaaaaaaa',
    source_commit: 'a'.repeat(40), nightly_sequence: 20261011,
  };
  const assetRequests: string[] = [];
  page.on('request', (request) => {
    if (request.url().includes('/api/updates/nightly-metadata') || request.url().includes('/releases/download/'))
      assetRequests.push(request.url());
  });
  await page.route('https://api.github.com/repos/Nonary/Vibepollo/releases?**', (route) =>
    route.fulfill({
      json: [{
        tag_name: tag,
        body: `<!-- nightly-identity: ${JSON.stringify(metadata)} -->\n`,
        draft: false,
        prerelease: true,
        html_url: `https://github.com/Nonary/Vibepollo/releases/tag/${tag}`,
        assets: ['VibepolloSetup.exe', 'nightly-metadata.json', 'SHA256SUMS'].map((name) => ({
          name,
          state: 'uploaded',
          size: 100,
        })),
      }],
    }),
  );
  await page.goto('/v2/');
  const notice = page.locator('.update-notice');
  await expect(notice.getByText(`Vibepollo ${tag} is available`)).toBeVisible();
  await notice.getByRole('button', { name: 'Dismiss this update' }).click();
  await expect(notice).toHaveCount(0);
  const checked = page.waitForResponse((response) => response.url().includes('/releases?per_page=100&page=1'));
  await page.reload();
  await (await checked).finished();
  await settleUpdaterRendering(page);
  await expect(notice).toHaveCount(0);
  expect(assetRequests).toEqual([]);
});

test('Windows prerelease install opts out persistently and still receives a stable update', async ({
  page,
}) => {
  await host(page, 'windows', { notify_pre_releases: 'disabled' }, true, undefined, {
    version: '2.0.0-beta.1',
  });
  await page.route('https://api.github.com/repos/Nonary/Vibepollo/releases?**', (route) =>
    route.fulfill({
      json: [
        {
          tag_name: '3.0.0-beta.1',
          draft: false,
          prerelease: true,
          assets: [{ name: 'VibepolloSetup.exe', state: 'uploaded', size: 100 }],
        },
        {
          tag_name: '2.0.0',
          draft: false,
          prerelease: false,
          assets: [{ name: 'VibepolloSetup.exe', state: 'uploaded', size: 100 }],
        },
      ],
    }),
  );
  await page.goto('/v2/');
  await expect(
    page.locator('.update-notice').getByText('Vibepollo 2.0.0 is available'),
  ).toBeVisible();
  await page.reload();
  await expect(
    page.locator('.update-notice').getByText('Vibepollo 2.0.0 is available'),
  ).toBeVisible();
});

// Both production UIs use one origin so their actual shared dismissal policy is exercised.
const updaterReleasesUrl = 'https://api.github.com/repos/Nonary/Vibepollo/releases';
type UpdaterReleaseFixture = {
  tag_name: string;
  draft: boolean;
  prerelease: boolean;
  html_url: string;
  name: string;
  body: string;
  assets: { name: string; state: string; size: number }[];
};

function updaterRelease(tag: string, assets = ['VibepolloSetup.exe']): UpdaterReleaseFixture {
  return {
    tag_name: tag,
    draft: false,
    prerelease: tag.startsWith('daily-') || /-(alpha|beta|rc)/.test(tag),
    html_url: `https://github.com/Nonary/Vibepollo/releases/tag/${tag}`,
    name: `Fixture ${tag}`,
    body: `Release notes for ${tag}`,
    assets: assets.map((name) => ({ name, state: 'uploaded', size: 100 })),
  };
}

function updaterDaily(tag: string, source = 'a') {
  const date = tag.replace('daily-', '').replaceAll('-', '');
  return {
    release: updaterRelease(tag, ['VibepolloSetup.exe', 'nightly-metadata.json', 'SHA256SUMS']),
    metadata: {
      schema_version: 1,
      channel: 'nightly',
      tag,
      version: `2.0.0-daily.d${date}+${source.repeat(12)}`,
      source_commit: source.repeat(40),
      nightly_sequence: Number(date),
    },
  };
}

async function mockUpdaterFeed(
  page: Page,
  releases: UpdaterReleaseFixture[],
  metadata: Record<string, unknown> = {},
) {
  const calls = { releases: 0, metadata: 0 };
  await page.route(`${updaterReleasesUrl}?**`, async (route) => {
    calls.releases++;
    const pageNumber = new URL(route.request().url()).searchParams.get('page');
    await route.fulfill({ json: pageNumber === '1' ? releases.map((release) => metadata[release.tag_name]
      ? { ...release, body: `<!-- nightly-identity: ${JSON.stringify(metadata[release.tag_name])} -->\n${release.body}` }
      : release) : [] });
  });
  await localRoute(page, '**/api/updates/nightly-metadata?**', async (route) => {
    calls.metadata++;
    const tag = new URL(route.request().url()).searchParams.get('tag') ?? '';
    await route.fulfill({ json: metadata[tag] ?? { invalid: true } });
  });
  return calls;
}

async function legacyUpdaterHost(
  page: Page,
  config: Record<string, unknown> = {},
  metadata: Record<string, unknown> = {},
  platform = 'windows',
) {
  const patches = await host(page, platform, config, true, undefined, metadata);
  const saves: Record<string, unknown>[] = [];
  const health = { vigem: 0, logs: 0 };
  let persistedSnapshot = false;
  await localRoute(page, '**/api/config', async (route) => {
    const method = route.request().method();
    if (method === 'GET' && persistedSnapshot) {
      // Native config::parse_config returns scalar values as strings after persistence.
      const stored = Object.fromEntries(Object.entries(config).map(([key, value]) => [
        key, typeof value === 'boolean' || typeof value === 'number' ? String(value) : value,
      ]));
      return route.fulfill({ json: { status: true, capture: 'kms', virtual_display_mode: 'per_client', ...stored } });
    }
    if (method !== 'PATCH' && method !== 'POST') return route.fallback();
    const body = route.request().postDataJSON() as Record<string, unknown>;
    if (method === 'PATCH') patches.push(body);
    else {
      saves.push(body);
      persistedSnapshot = true;
      // POST rewrites native configuration. A dropped opt-out must not survive by accidental merging.
      for (const key of Object.keys(config)) delete config[key];
    }
    for (const [key, value] of Object.entries(body)) {
      if (value === null || value === '') delete config[key];
      else config[key] = value;
    }
    await route.fulfill({ json: { status: true, appliedNow: true, deferred: false, restartRequired: false } });
  });
  await localRoute(page, '**/api/auth/sessions', (route) => route.fulfill({ json: { status: true, sessions: [] } }));
  await localRoute(page, '**/api/health/vigem', (route) => {
    health.vigem++;
    return route.fulfill({ json: { installed: true, required: true, version: 'fixture' } });
  });
  await localRoute(page, '**/api/health/vulkan-hdr-layer', (route) => route.fulfill({ json: { installed: true, enabled: false } }));
  await localRoute(page, '**/api/health/crashdump', (route) => route.fulfill({ json: { available: false } }));
  await localRoute(page, '**/api/display/golden_status', (route) => route.fulfill({ json: {
    exists: true, out_of_date: false, needs_layout_upgrade: false, restore_failure_count: 0,
  } }));
  await localRoute(page, '**/api/playnite/status', (route) => route.fulfill({ json: {
    installed: true, active: false, update_available: false,
    installed_version: 'fixture', packaged_version: 'fixture',
  } }));
  await localRoute(page, '**/api/frame-limiter/status', (route) => route.fulfill({ json: {
    enabled: false, configured_provider: 'rtss', active_provider: '',
    rtss_available: false, nvidia_available: false, mangohud_available: false,
  } }));
  await localRoute(page, '**/api/lossless_scaling/status', (route) => route.fulfill({ json: {} }));
  await localRoute(page, '**/api/logs', (route) => {
    health.logs++;
    return route.fulfill({ contentType: 'text/plain', body: '' });
  });
  return { patches, saves, health };
}

function classicUpdaterUrl() {
  const url = process.env.VIBEPOLLO_LEGACY_TEST_URL;
  test.skip(!url, 'Set VIBEPOLLO_LEGACY_TEST_URL to a classic production build on the v2 test origin.');
  const baseUrl = test.info().project.use.baseURL;
  if (baseUrl) expect(new URL(url!).origin).toBe(new URL(baseUrl).origin);
  return url!;
}

function legacyUpdaterCompleted(page: Page) {
  return page.waitForResponse((response) => new URL(response.url()).pathname === '/api/logs');
}

async function stallUpdaterReleaseBody(page: Page) {
  await page.addInitScript(() => {
    const state = { started: 0, aborted: 0 };
    Object.assign(window, { __vibepolloUpdaterBody: state });
    const originalFetch = window.fetch.bind(window);
    window.fetch = async (input: RequestInfo | URL, init?: RequestInit) => {
      const response = await originalFetch(input, init);
      const url = new URL(input instanceof Request ? input.url : String(input), location.href);
      if (url.hostname !== 'api.github.com' || !url.pathname.endsWith('/releases') || !url.searchParams.has('page')) return response;
      const signal = init?.signal ?? (input instanceof Request ? input.signal : undefined);
      // Only release API body decoding is controlled; selection stays real.
      response.json = () => {
        state.started++;
        return new Promise((_, reject) => {
          const aborted = () => {
            state.aborted++;
            reject(signal?.reason ?? new DOMException('Fixture metadata body aborted', 'AbortError'));
          };
          if (signal?.aborted) aborted();
          else signal?.addEventListener('abort', aborted, { once: true });
        });
      };
      return response;
    };
  });
}

async function updaterBodyState(page: Page) {
  return page.evaluate(() => (window as typeof window & {
    __vibepolloUpdaterBody?: { started: number; aborted: number };
  }).__vibepolloUpdaterBody);
}

test('Apollo updater paginates before selecting a complete release', async ({ page }) => {
  await host(page, 'windows');
  const pages: string[] = [];
  await page.route(`${updaterReleasesUrl}?**`, (route) => {
    const pageNumber = new URL(route.request().url()).searchParams.get('page') ?? '';
    pages.push(pageNumber);
    return route.fulfill({ json: pageNumber === '1'
      ? Array.from({ length: 100 }, (_, index) => updaterRelease(`0.0.${index}`))
      : [updaterRelease('v2.0.0')] });
  });
  await page.goto('/v2/');
  await expect(page.locator('.update-notice')).toContainText('Vibepollo 2.0.0 is available');
  expect(pages).toEqual(['1', '2']);
});

test('Apollo interval zero suppresses automatic updater requests', async ({ page }) => {
  await page.clock.install();
  await host(page, 'windows', { update_check_interval: 0 });
  const daily = updaterDaily('daily-2026-10-11');
  const calls = await mockUpdaterFeed(page, [daily.release], { [daily.metadata.tag]: daily.metadata });
  await page.goto('/v2/');
  await expect(page.getByRole('heading', { name: 'Ready to stream' })).toBeVisible();
  await settleUpdaterRendering(page);
  await page.clock.fastForward(300001);
  expect(calls).toEqual({ releases: 0, metadata: 0 });
  await expect(page.locator('.update-notice')).toHaveCount(0);
});

test('Apollo updater interval zero still permits an explicit retry without resuming its timer', async ({ page }) => {
  await page.clock.install();
  const config = { update_check_interval: 1 };
  await host(page, 'windows', config);
  let calls = 0;
  await page.route(`${updaterReleasesUrl}?**`, (route) => {
    calls++;
    return route.fulfill(calls === 1
      ? { status: 403, json: { message: 'fixture unavailable' } }
      : { json: [updaterRelease('v2.0.0')] });
  });
  await page.goto('/v2/');
  const notice = page.locator('.update-notice');
  await expect(notice.getByText('Release information is unavailable. Try again later.')).toBeVisible();
  config.update_check_interval = 0;
  await notice.getByRole('button', { name: 'Check for updates', exact: true }).click();
  await expect(notice.getByText('Vibepollo 2.0.0 is available')).toBeVisible();
  expect(calls).toBe(2);
  await page.clock.fastForward(300001);
  expect(calls).toBe(2);
});

for (const preference of [false, 'false', 'disabled', 0, '0']) {
  test(`legacy updater preserves explicit ${JSON.stringify(preference)} after PATCH and full save`, async ({ page }) => {
    const legacyUrl = classicUpdaterUrl();
    const config: Record<string, unknown> = { notify_pre_releases: preference, sunshine_name: 'Original host' };
    const { patches, saves } = await legacyUpdaterHost(page, config, { version: '2.0.0-beta.1' });
    const daily = updaterDaily('daily-2026-10-11');
    const calls = await mockUpdaterFeed(page, [daily.release, updaterRelease('3.0.0-beta.1')], { [daily.metadata.tag]: daily.metadata });
    const firstCheck = legacyUpdaterCompleted(page);
    await page.goto(legacyUrl + '/');
    await (await firstCheck).finished();
    const check = page.getByRole('button', { name: 'Check for updates', exact: true });
    await expect.poll(() => calls.releases).toBe(1);
    await expect(check).toBeEnabled();
    await expect(page.locator('.dashboard-release-alert')).toHaveCount(0);
    expect(calls.metadata).toBe(0);
    await page.locator('.dashboard-page a[href="/settings"]').click();
    const name = page.locator('input#sunshine_name, #sunshine_name input');
    await expect(name).toBeVisible();
    await name.fill('Renamed host');
    await name.blur();
    await expect.poll(() => patches.some((patch) => patch.sunshine_name === 'Renamed host')).toBe(true);
    expect(patches.every((patch) => !Object.hasOwn(patch, 'notify_pre_releases'))).toBe(true);
    await page.locator('#global_prep_cmd').getByRole('button', { name: '+ Add', exact: true }).click();
    await page.locator('textarea#global_prep_cmd_do_0, #global_prep_cmd_do_0 textarea').fill('fixture-only-command');
    await page.getByRole('button', { name: 'Save', exact: true }).click();
    await expect.poll(() => saves.length).toBe(1);
    expect(saves[0]['notify_pre_releases']).toBe(preference);
    expect(saves[0]['sunshine_name']).toBe('Renamed host');
    expect(config['notify_pre_releases']).toBe(preference);
    const reloadedCheck = legacyUpdaterCompleted(page);
    await page.goto(legacyUrl + '/');
    await (await reloadedCheck).finished();
    await expect.poll(() => calls.releases).toBe(2);
    await expect(check).toBeEnabled();
    await expect(page.locator('.dashboard-release-alert')).toHaveCount(0);
    expect(calls.metadata).toBe(0);
  });
}

test('legacy updater orders embedded daily versions and shares dismissal across reload and UI switch', async ({ page }) => {
  const legacyUrl = classicUpdaterUrl();
  await legacyUpdaterHost(page, {}, { version: '2.0.0-daily.d20261009+cccccccccccc' });
  const older = updaterDaily('daily-2026-10-10', 'b');
  const newer = updaterDaily('daily-2026-10-11');
  const later = updaterDaily('daily-2026-10-12', 'd');
  // The older release is deliberately first; the shared helper must use embedded lineage.
  const releases = [older.release, newer.release];
  const metadata = { [older.metadata.tag]: older.metadata, [newer.metadata.tag]: newer.metadata, [later.metadata.tag]: later.metadata };
  await mockUpdaterFeed(page, releases, metadata);
  await page.goto(legacyUrl + '/');
  const notice = page.locator('.dashboard-release-alert');
  await expect(notice).toHaveCount(1);
  await expect(notice).toContainText(newer.metadata.version);
  const downloadLink = notice.getByRole('link', { name: /\bDownload$/ });
  await expect(downloadLink).toHaveCount(1);
  await expect(downloadLink).toHaveAttribute('href', newer.release.html_url);
  const releaseNotesButton = notice.getByRole('button', { name: /\bRelease Notes$/ });
  await expect(releaseNotesButton).toHaveCount(1);
  await releaseNotesButton.click();
  await expect(notice).toContainText(newer.release.body);
  await notice.getByRole('button', { name: 'Dismiss this update', exact: true }).click();
  await expect(notice).toHaveCount(0);
  const reloadedCheck = legacyUpdaterCompleted(page);
  await page.reload();
  await (await reloadedCheck).finished();
  await expect(page.getByRole('button', { name: 'Check for updates', exact: true })).toBeEnabled();
  await expect(notice).toHaveCount(0);
  const switchedCheck = page.waitForResponse((response) => {
    const url = new URL(response.url());
    return url.hostname === 'api.github.com' && url.pathname.endsWith('/releases') && url.searchParams.has('page');
  });
  await page.locator('.dashboard-page a[href="/v2/"]').click();
  await (await switchedCheck).finished();
  await expect(page.getByRole('heading', { name: 'Ready to stream' })).toBeVisible();
  await settleUpdaterRendering(page);
  await expect(page.locator('.update-notice')).toHaveCount(0);
  releases.unshift(later.release);
  await page.reload();
  await expect(page.locator('.update-notice')).toContainText(`Vibepollo ${later.metadata.tag} is available`);
  await expect(page.locator('.update-notice').getByRole('link', { name: 'Read release notes' })).toHaveAttribute('href', later.release.html_url);
  await page.locator('.sidebar__legacy[href="/"]').click();
  await expect(notice).toContainText(later.metadata.version);
});

test('legacy updater rejects malformed metadata and incomplete or wrong-platform assets', async ({ page }) => {
  const legacyUrl = classicUpdaterUrl();
  await legacyUpdaterHost(page, {}, { version: '1.9.0' });
  const malformed = updaterDaily('daily-2026-10-11');
  const incomplete = updaterDaily('daily-2026-10-12', 'b');
  incomplete.release.assets = incomplete.release.assets.filter((asset) => asset.name !== 'SHA256SUMS');
  const wrongPlatform = updaterDaily('daily-2026-10-13', 'c');
  wrongPlatform.release.assets[0].name = 'Vibepollo-aarch64.AppImage';
  const empty = updaterDaily('daily-2026-10-14', 'd');
  empty.release.assets[0].size = 0;
  const calls = await mockUpdaterFeed(page, [malformed.release, incomplete.release, wrongPlatform.release, empty.release], {
    [malformed.metadata.tag]: { ...malformed.metadata, source_commit: 'b'.repeat(40) },
  });
  const completedCheck = legacyUpdaterCompleted(page);
  await page.goto(legacyUrl + '/');
  await (await completedCheck).finished();
  await expect.poll(() => calls.releases).toBe(1);
  expect(calls.metadata).toBe(0);
  await expect(page.getByRole('button', { name: 'Check for updates', exact: true })).toBeEnabled();
  await expect(page.locator('.dashboard-release-alert')).toHaveCount(0);
});

for (const [architecture, available] of [['x86_64', true], ['aarch64', false], ['unknown', false], [undefined, false]] as const) {
  test(`legacy updater checks Linux package architecture ${String(architecture)}`, async ({ page }) => {
    const legacyUrl = classicUpdaterUrl();
    await legacyUpdaterHost(page, {}, { version: '1.9.0', architecture }, 'linux');
    const daily = updaterDaily('daily-2026-10-11');
    daily.release.assets[0].name = 'vibepollo-2.0.0-1-x86_64.pkg.tar.zst';
    const calls = await mockUpdaterFeed(page, [daily.release], { [daily.metadata.tag]: daily.metadata });
    const completedCheck = legacyUpdaterCompleted(page);
    await page.goto(legacyUrl + '/');
    await (await completedCheck).finished();
    if (available) await expect(page.locator('.dashboard-release-alert')).toContainText(daily.metadata.version);
    else await expect(page.locator('.dashboard-release-alert')).toHaveCount(0);
    expect(calls).toEqual({ releases: 1, metadata: 0 });
  });
}

test('legacy updater interval zero retains health and logs and permits one forced check', async ({ page }) => {
  const legacyUrl = classicUpdaterUrl();
  await page.clock.install();
  const { health } = await legacyUpdaterHost(page, { update_check_interval: 0, controller: 'enabled' });
  const daily = updaterDaily('daily-2026-10-11');
  const calls = await mockUpdaterFeed(page, [daily.release], { [daily.metadata.tag]: daily.metadata });
  const completedCheck = legacyUpdaterCompleted(page);
  await page.goto(legacyUrl + '/');
  await (await completedCheck).finished();
  await expect.poll(() => health.vigem > 0 && health.logs > 0).toBe(true);
  expect(calls).toEqual({ releases: 0, metadata: 0 });
  await page.getByRole('button', { name: 'Check for updates', exact: true }).click();
  await expect(page.locator('.dashboard-release-alert')).toContainText(daily.metadata.version);
  expect(calls).toEqual({ releases: 1, metadata: 0 });
  await page.clock.fastForward(300001);
  expect(calls).toEqual({ releases: 1, metadata: 0 });
});

for (const ui of ['legacy', 'v2'] as const) {
  test(`${ui} updater aborts release API body on unmount without resuming its timer`, async ({ page }) => {
    const legacyUrl = ui === 'legacy' ? classicUpdaterUrl() : undefined;
    await page.clock.install();
    await stallUpdaterReleaseBody(page);
    if (ui === 'legacy') await legacyUpdaterHost(page);
    else await host(page, 'windows');
    let authenticated = true;
    await localRoute(page, '**/api/auth/status', (route) => route.fulfill({ json: {
      authenticated, login_required: !authenticated, credentials_configured: true,
    } }));
    await localRoute(page, '**/api/auth/logout', (route) => {
      authenticated = false;
      return route.fulfill({ json: { status: true } });
    });
    const daily = updaterDaily('daily-2026-10-11');
    // A newer selectable stable release exposes swallowed cancellation after metadata enrichment.
    const calls = await mockUpdaterFeed(page, [daily.release, updaterRelease('v3.0.0')], { [daily.metadata.tag]: daily.metadata });
    await page.goto(ui === 'legacy' ? legacyUrl + '/' : '/v2/');
    await expect.poll(async () => (await updaterBodyState(page))?.started).toBe(1);
    if (ui === 'legacy') {
      await page.locator('.dashboard-page a[href="/settings"]').click();
      await expect(page.locator('.dashboard-page')).toHaveCount(0);
    } else {
      await page.locator('.sidebar__logout').click();
      await expect(page.locator('.app-shell')).toHaveCount(0);
    }
    await expect.poll(async () => (await updaterBodyState(page))?.aborted).toBe(1);
    await page.clock.fastForward(300001);
    expect(calls).toEqual({ releases: 1, metadata: 0 });
    await expect(page.locator('.update-notice, .dashboard-release-alert')).toHaveCount(0);
  });
}

for (const ui of ['legacy', 'v2'] as const) {
  test(`${ui} updater interval zero release-body timeout cannot select stable or resume checks`, async ({ page }) => {
    const legacyUrl = ui === 'legacy' ? classicUpdaterUrl() : undefined;
    await page.clock.install();
    await stallUpdaterReleaseBody(page);
    const config = { update_check_interval: ui === 'legacy' ? 0 : 1 };
    if (ui === 'legacy') await legacyUpdaterHost(page, config);
    else await host(page, 'windows', config);
    const daily = updaterDaily('daily-2026-10-11');
    const calls = await mockUpdaterFeed(page, [daily.release, updaterRelease('v3.0.0')], { [daily.metadata.tag]: daily.metadata });
    if (ui === 'v2') {
      await page.route(`${updaterReleasesUrl}?**`, (route) => route.fulfill({ status: 403, json: { message: 'fixture unavailable' } }));
    }
    await page.goto(ui === 'legacy' ? legacyUrl + '/' : '/v2/');
    const check = page.getByRole('button', { name: 'Check for updates', exact: true });
    await expect(check).toBeVisible();
    await expect(check).toBeEnabled();
    config.update_check_interval = 0;
    if (ui === 'v2') await page.unroute(`${updaterReleasesUrl}?**`);
    // Restore the controlled release route removed together with the one-off failure route.
    if (ui === 'v2') await page.route(`${updaterReleasesUrl}?**`, (route) => {
      calls.releases++;
      return route.fulfill({ json: [daily.release, updaterRelease('v3.0.0')] });
    });
    await check.click();
    await expect.poll(async () => (await updaterBodyState(page))?.started).toBe(1);
    await page.clock.fastForward(15001);
    await expect.poll(async () => (await updaterBodyState(page))?.aborted).toBe(1);
    await expect(check).toBeEnabled();
    await expect(page.getByText(ui === 'legacy'
      ? 'Update check unavailable. Try again.'
      : 'Release information is unavailable. Try again later.', { exact: true })).toBeVisible();
    await expect(page.locator('.update-notice a')).toHaveCount(0);
    await expect(page.locator('.dashboard-release-alert')).toHaveCount(0);
    await page.clock.fastForward(300001);
    expect(calls).toEqual({ releases: 1, metadata: 0 });
  });
}
