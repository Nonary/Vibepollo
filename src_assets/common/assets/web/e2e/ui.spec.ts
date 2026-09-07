import { test, expect, type Page } from '@playwright/test';

async function host(
  page: Page,
  platform = 'linux',
  config: Record<string, unknown> = {},
  ready = true,
  providers: Record<string, boolean> = {},
) {
  const patches: Record<string, unknown>[] = [];
  await page.route('**/api/**', async (route) => {
    const path = new URL(route.request().url()).pathname;
    if (!path.startsWith('/api/')) {
      await route.continue();
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

test('HTTP save rejection retains the draft', async ({ page }) => {
  await host(page);
  await page.route('**/api/config', async (route) => {
    if (route.request().method() === 'PATCH') await route.fulfill({ json: { status: false } });
    else await route.fallback();
  });
  await page.goto('/v2/settings');
  await page.locator('#setting-stream_audio').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.locator('#setting-stream_audio')).not.toBeChecked();
  await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toBeEnabled();
});

for (const width of [390, 768, 1100, 1440]) {
  test(`settings and save bar fit at ${width}px`, async ({ page }) => {
    await host(page);
    await page.setViewportSize({ width, height: 1000 });
    await page.goto('/v2/settings');
    await expect(page.locator('#setting-stream_audio')).toBeVisible();
    await page.locator('#setting-stream_audio').uncheck();
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await expect(page.locator('.save-bar')).toBeVisible();
    const save = await page.locator('.save-bar').boundingBox();
    expect(save!.x).toBeGreaterThanOrEqual(0);
    expect(save!.x + save!.width).toBeLessThanOrEqual(width);
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/everyday-${width}.png`,
      fullPage: true,
    });
  });
}

test('bulk unpair requires a confirmation and calls the existing endpoint', async ({ page }) => {
  await host(page);
  let calls = 0;
  await page.route('**/api/clients/unpair-all', async (route) => {
    calls++;
    await route.fulfill({ json: { status: true } });
  });
  await page.goto('/v2/devices');
  await page.getByRole('button', { name: 'Unpair all devices' }).click();
  expect(calls).toBe(0);
  await page.getByRole('dialog').getByRole('button', { name: 'Unpair all devices' }).click();
  await expect.poll(() => calls).toBe(1);
});

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
    await page.addInitScript((theme) => localStorage.setItem('vibeshine.theme', theme), theme);
    await page.goto('/v2/settings');
    await expect(page.locator('html')).toHaveAttribute('data-theme', theme);
    await page.locator('#setting-stream_audio').focus();
    await page.keyboard.press('Space');
    await expect(page.locator('#setting-stream_audio')).not.toBeChecked();
    await page.evaluate(() => window.scrollTo(0, 0));
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/everyday-${theme}.png`,
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

test('Linux adapters remain editable while unsupported provider destinations stay hidden', async ({
  page,
}) => {
  const patches = await host(page);
  await page.goto('/v2/settings?category=display#setting-adapter_name');
  await page.locator('#setting-adapter_name').fill('/dev/dri/renderD129');
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toEqual({ adapter_name: '/dev/dri/renderD129' });
  await page.getByRole('searchbox', { name: 'Search settings' }).fill('lutris');
  await expect(page.locator('.settings-destinations a')).toHaveCount(0);
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
  await page.route('**/api/config', async (route) => {
    if (failed) await route.fulfill({ status: 503, json: { status: false } });
    else await route.fallback();
  });
  await page.goto('/v2/settings');
  await expect(page.getByRole('alert')).toBeVisible();
  await expect(page.locator('#setting-stream_audio')).toHaveCount(0);
  failed = false;
  await page.getByRole('button', { name: 'Reload', exact: true }).click();
  await expect(page.locator('#setting-stream_audio')).toBeVisible();
});

test('edits made during a settings save remain unsaved', async ({ page }) => {
  await host(page);
  let finish!: () => void;
  const pending = new Promise<void>((resolve) => {
    finish = resolve;
  });
  let started = false;
  await page.route('**/api/config', async (route) => {
    if (route.request().method() !== 'PATCH') {
      await route.fallback();
      return;
    }
    started = true;
    await pending;
    await route.fulfill({ json: { status: true } });
  });
  await page.goto('/v2/settings');
  await page.locator('#setting-stream_audio').uncheck();
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect.poll(() => started).toBe(true);
  await page.locator('#setting-mouse').uncheck();
  finish();
  await expect(page.getByRole('button', { name: 'Save changes', exact: true })).toBeEnabled();
  await expect(page.locator('#setting-mouse')).not.toBeChecked();
  await expect(page.locator('.save-bar')).toContainText('1 unsaved change');
});

test('physical screen controls fit with long output names at intermediate widths', async ({
  page,
}) => {
  await host(page, 'windows', {
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
  await page.locator('#setting-stream_audio').focus();
  await page.keyboard.press('Space');
  await expect(page.locator('#setting-stream_audio')).not.toBeChecked();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.getByRole('button', { name: 'Save changes', exact: true }).click();
  await expect(page.locator('.save-bar')).toHaveCount(0);
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

test('unknown readiness and unavailable utilization do not render as successful measurements', async ({
  page,
}) => {
  await host(page);
  await page.route('**/api/metadata', (route) => route.fulfill({ json: { platform: 'linux' } }));
  await page.route('**/api/host/stats', (route) =>
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
  await page.route('**/api/apps', (route) =>
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
  await page.route('**/api/apps/*/cover', (route) => route.fulfill({ status: 404, body: '' }));
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
  await page.route('**/api/host/stats', (route) =>
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
    expect(requests.some((path) => /^\/api\/(steam|lutris)\//.test(path))).toBe(false);
    if (platform === 'linux') {
      await expect(page.getByRole('button', { name: 'Browse games', exact: true })).toHaveCount(0);
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
    await page.route('**/api/apps', (route) => {
      if (route.request().method() === 'POST') {
        saved.push(route.request().postDataJSON());
        return route.fulfill({
          status: 400,
          json: { status: false, error: 'Keep editor open for assertions' },
        });
      }
      return route.fulfill({ json: { apps: [] } });
    });
    await page.route('**/api/playnite/status', (route) =>
      route.fulfill({ json: { installed: true, active: true } }),
    );
    await page.route('**/api/playnite/games', (route) =>
      route.fulfill({ json: [{ id: 'playnite-portal', name: 'Portal', installed: true }] }),
    );
    await page.route('**/api/steam/games*', (route) =>
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
    await page.route('**/api/lutris/games', (route) =>
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
  await page.route('**/api/playnite/status', (route) =>
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
  await page.route('**/api/steam/games*', async (route) => {
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
  await page.route('**/api/apps', (route) => {
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
  await page.route('**/api/metadata', (route) =>
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

test('unmigrated Linux services are neither offered nor called', async ({ page }) => {
  await host(page, 'linux');
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
  await page.goto('/v2/settings');
  await expect(
    page.locator('#setting-virtual_display_mode, #setting-frame_limiter_provider'),
  ).toHaveCount(0);
  await expect(page.locator('.linux-capture')).toHaveCount(0);
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
    await page.route('**/api/apps', async (route) => {
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
      .screenshot({ path: `/tmp/vibepollo-ui-results/app-behavior-${width}.png` });
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
  await page.route('**/api/apps', async (route) => {
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
    await page.route('**/api/playnite/status', (route) =>
      route.fulfill({ json: { installed: true, active: true } }),
    );
    await page.route('**/api/playnite/games', (route) =>
      route.fulfill({ json: [{ id: 'pn-portal', name: 'Portal', installed: true }] }),
    );
    await page.route('**/api/steam/games*', (route) =>
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
    await page.route('**/api/lutris/games', (route) =>
      route.fulfill({ json: { games: [{ id: 9, name: 'Portal', directory: '/games/portal' }] } }),
    );
    const saved: Record<string, unknown>[] = [];
    await page.route('**/api/apps', (route) => {
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
    await page.route('**/api/apps/*/cover*', (route) =>
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
  await page.route('**/api/playnite/status', route => route.fulfill({ json: { installed: false } }));
  let synced = false;
  await page.route('**/api/steam/force_sync', route => {
    synced = true;
    return route.fulfill({ json: { status: true } });
  });
  await page.route('**/api/apps', route => route.fulfill({ json: {
    apps: synced ? [{ name: 'Synced Steam game', uuid: 'steam-test', 'steam-id': '42' }] : [],
  } }));
  await page.goto('/v2/library');
  await page.getByRole('button', { name: 'Setup Game Library Integration', exact: true }).click();
  const dialog = page.getByRole('dialog');
  await dialog.getByRole('checkbox', { name: 'Steam', exact: true }).check();
  await dialog.getByRole('button', { name: 'Next: Library settings' }).click();
  await dialog.getByRole('spinbutton', { name: 'Recent games', exact: true }).fill('6');
  await dialog.getByRole('button', { name: 'Save settings', exact: true }).click();
  await dialog.getByRole('button', { name: 'Done', exact: true }).click();
  await expect(page.getByText('Synced Steam game', { exact: true }).first()).toBeVisible();
  await expect(page.getByRole('button', { name: 'Setup Game Library Integration', exact: true })).toHaveCount(0);
  await page.reload();
  await page.getByRole('button', { name: 'Library manager settings', exact: true }).click();
  await dialog.getByRole('button', { name: 'Next: Library settings' }).click();
  await expect(dialog.getByRole('spinbutton', { name: 'Recent games', exact: true })).toHaveValue('6');
});
