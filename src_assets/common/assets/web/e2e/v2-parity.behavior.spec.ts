import { expect, test, type Page } from '@playwright/test';

type HostFixture = {
  platform?: 'windows' | 'linux';
  metadata?: Record<string, unknown>;
  metadataUnavailable?: boolean;
  config?: Record<string, unknown>;
  restartRequired?: boolean;
};

async function installHost(page: Page, options: HostFixture = {}) {
  const platform = options.platform ?? 'linux';
  const config = {
    capture: 'kms',
    virtual_display_mode: 'per_client',
    stream_audio: true,
    wayland_hdr_compatibility: false,
    frame_limiter_provider: 'auto',
    mangohud_limiter_method: 'late',
    ...options.config,
  };
  const patches: Record<string, unknown>[] = [];

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
      body = { csrf_token: 'parity-token' };
    } else if (path === '/api/config' && request.method() === 'GET') {
      body = { status: true, ...config };
    } else if (path === '/api/config' && request.method() === 'PATCH') {
      const patch = request.postDataJSON() as Record<string, unknown>;
      patches.push(patch);
      Object.assign(config, patch);
      body = { status: true, restartRequired: options.restartRequired ?? false };
    } else if (path === '/api/metadata') {
      if (options.metadataUnavailable) {
        await route.fulfill({ status: 503, json: { error: 'metadata unavailable' } });
        return;
      }
      body = options.metadata ?? {
        platform,
        version: '1.0.0',
        windows_build_number: platform === 'windows' ? 26100 : undefined,
        virtual_display: {
          backend: 'kscreen-vkms',
          capable: true,
          ready: true,
        },
        capture_status: {
          configured_backend: platform === 'linux' ? 'kms' : 'wgc',
          observed_backend: 'kms',
          managed_event_driven: true,
          virtual_display_configured: true,
        },
      };
    } else if (path === '/api/display-devices') {
      body = [{ device_id: 'HDMI-A-1', friendly_name: 'Test monitor', info: { active: true } }];
    } else if (path === '/api/display/golden_status') {
      body = { exists: false };
    } else if (path === '/api/auth/sessions') {
      body = { sessions: [] };
    } else if (path === '/api/apps') {
      body = { apps: [] };
    } else if (path === '/api/health/crashdump') {
      body = { available: false };
    }
    await route.fulfill({ json: body });
  });
  await page.route('**/assets/changelog.json', (route) => route.fulfill({ json: { releases: [] } }));
  return patches;
}

test('Linux maintenance renders Linux actions without Windows recovery or support sections', async ({
  page,
}) => {
  await installHost(page, { platform: 'linux' });
  await page.goto('/v2/maintenance');

  await expect(page.getByRole('heading', { name: 'Maintenance' })).toBeVisible();
  await expect(page.getByRole('link', { name: 'Display settings' })).toBeVisible();
  await expect(page.getByRole('link', { name: 'Open and download logs' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Virtual display recovery' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Terminate virtual display' })).toBeVisible();
  await expect(page.getByRole('heading', { name: 'Support bundles' })).toHaveCount(0);
});

test('Settings renders and saves Linux HDR and MangoHUD choices through real controls', async ({
  page,
}) => {
  const patches = await installHost(page, { platform: 'linux' });
  await page.goto('/v2/settings?category=display');

  const reload = page.getByRole('button', { name: 'Reload' });
  await expect(reload).toBeEnabled();
  const waylandHdr = page.locator('#setting-wayland_hdr_compatibility');
  await expect(waylandHdr).toBeVisible();
  await expect(waylandHdr).not.toBeChecked();
  await waylandHdr.check();
  await expect(reload).toBeDisabled();
  await page.getByRole('button', { name: 'Save changes' }).click();
  await expect.poll(() => patches.length).toBe(1);
  expect(patches[0]).toMatchObject({ wayland_hdr_compatibility: true });

  await page.goto('/v2/settings?category=pacing');
  await page.locator('#setting-frame_limiter_provider').selectOption('mangohud');
  const limiterMethod = page.locator('#setting-mangohud_limiter_method');
  await expect(limiterMethod).toBeVisible();
  await limiterMethod.selectOption('early');
  await page.getByRole('button', { name: 'Save changes' }).click();

  await expect.poll(() => patches.length).toBe(2);
  expect(patches[1]).toMatchObject({
    frame_limiter_provider: 'mangohud',
    mangohud_limiter_method: 'early',
  });
});

test('Settings reports missing host metadata and unavailable Linux virtual displays', async ({
  page,
}) => {
  await installHost(page, { platform: 'linux', metadataUnavailable: true });
  await page.goto('/v2/settings');
  await expect(page.getByText('Some host details are unavailable', { exact: true })).toBeVisible();

  await page.close();
  const displayPage = await page.context().newPage();
  await installHost(displayPage, {
    platform: 'linux',
    metadata: {
      platform: 'linux',
      virtual_display: { backend: 'kscreen-vkms', capable: false, ready: false },
      capture_status: { virtual_display_configured: false },
    },
  });
  await displayPage.goto('/v2/settings?category=display');
  await expect(
    displayPage.getByText('Virtual displays are not ready on this host', { exact: true }),
  ).toBeVisible();
  await displayPage.close();
});

test('Settings retains save and restart actions after a backend requests restart', async ({
  page,
}) => {
  const patches = await installHost(page, { platform: 'linux', restartRequired: true });
  let restartRequested = false;
  let releaseRestart!: () => void;
  const restartPending = new Promise<void>((resolve) => { releaseRestart = resolve; });
  await page.route('**/api/restart', async (route) => {
    restartRequested = true;
    await restartPending;
    await route.fulfill({ json: { status: true } });
  });
  await page.goto('/v2/settings?category=audio');
  await page.locator('#setting-stream_audio').uncheck();
  await expect(page.getByRole('button', { name: 'Reload' })).toBeDisabled();
  await page.getByRole('button', { name: 'Save changes' }).click();

  await expect.poll(() => patches.length).toBe(1);
  await expect(page.getByRole('button', { name: 'Restart now' })).toBeVisible();
  await page.getByRole('button', { name: 'Restart now' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  try {
    await page.getByRole('dialog').getByRole('button', { name: 'Continue' }).click();
    await expect.poll(() => restartRequested).toBe(true);
    const restarting = page.getByRole('button', { name: /Restarting/i });
    await expect(restarting).toBeDisabled();
    await expect(restarting).toHaveAttribute('aria-busy', 'true');
  } finally {
    releaseRestart();
  }
});

test('Linux settings enumerate physical displays and reset persisted display state', async ({
  page,
}) => {
  await installHost(page, {
    platform: 'linux',
    config: { virtual_display_mode: 'disabled' },
  });
  let displayRequests = 0;
  let resetRequests = 0;
  await page.route('**/api/display-devices**', async (route) => {
    displayRequests += 1;
    await route.fulfill({
      json: [{ device_id: 'DP-1', friendly_name: 'Studio display', info: { active: true } }],
    });
  });
  await page.route('**/api/reset-display-device-persistence', async (route) => {
    resetRequests += 1;
    await route.fulfill({ json: { status: true } });
  });

  await page.goto('/v2/settings?category=everyday');
  const display = page.locator('#setting-output_name');
  await expect(display).toHaveJSProperty('tagName', 'SELECT');
  await display.focus();
  await expect(display.locator('option[value="DP-1"]')).toContainText('Studio display - DP-1');
  await display.selectOption('DP-1');
  await expect(page.getByRole('button', { name: 'Save changes' })).toBeVisible();

  await page.goto('/v2/settings?category=display');
  await page.getByRole('button', { name: 'Clear display state' }).click();
  await page.getByRole('dialog').getByRole('button', { name: 'Continue' }).click();
  await expect.poll(() => resetRequests).toBe(1);
  expect(displayRequests).toBeGreaterThan(0);
});

test('device save merges local display and command edits with the latest server draft', async ({
  page,
}) => {
  await installHost(page, { platform: 'linux' });

  const original = {
    uuid: 'alpha',
    name: 'Living room',
    enabled: true,
    connected: true,
    allow_client_commands: true,
    do: [{ cmd: 'old-do', elevated: false }],
    undo: [{ cmd: 'old-undo', elevated: false }],
    display_mode: '1920x1080x60',
  };
  let listCalls = 0;
  let updatePayload: Record<string, unknown> | undefined;
  await page.route('**/api/clients/list', async (route) => {
    listCalls += 1;
    const device = listCalls === 1 ? original : { ...original, name: 'Latest server name' };
    await route.fulfill({ json: { status: true, platform: 'linux', named_certs: [device] } });
  });
  await page.route('**/api/clients/update', async (route) => {
    updatePayload = route.request().postDataJSON() as Record<string, unknown>;
    await route.fulfill({ json: { status: true } });
  });

  await page.goto('/v2/devices');
  await page.getByRole('button', { name: 'Edit device' }).click();
  await page.locator('#client-alpha-display-mode').fill('2560x1440x144');
  await page.locator('#client-alpha-commands-do-0').fill('new-do');
  await page.locator('#client-alpha-commands-undo-0').fill('new-undo');
  await page.locator('#client-alpha-commands-allow').uncheck();
  await page.getByRole('button', { name: 'Save device' }).click();

  await expect.poll(() => updatePayload).toMatchObject({
    name: 'Latest server name',
    display_mode: '2560x1440x144',
    allow_client_commands: false,
    do: [{ cmd: 'new-do', elevated: false }],
    undo: [{ cmd: 'new-undo', elevated: false }],
  });
});

for (const viewport of [
  { width: 1280, height: 720 },
  { width: 390, height: 844 },
]) {
  test(`device save stays in view when changing stream permission at ${viewport.width}px`, async ({
    page,
  }) => {
    await page.setViewportSize(viewport);
    await installHost(page, { platform: 'windows' });
    const stored: Record<string, unknown> = {
      uuid: 'alpha',
      name: 'Living room',
      perm: 0x071f1f00,
      enable_legacy_ordering: true,
      connected: false,
      display_mode: '2560x1440x120',
      hdr_profile: 'living-room.icc',
      prefer_10bit_sdr: true,
      config_overrides: { bitrate: '25000' },
      allow_client_commands: false,
      do: [{ cmd: 'keep-do', elevated: true }],
      undo: [{ cmd: 'keep-undo', elevated: false }],
    };
    let latestIdentity = false;
    const updates: Record<string, unknown>[] = [];
    let releaseUpdate!: () => void;
    const updatePending = new Promise<void>((resolve) => {
      releaseUpdate = resolve;
    });
    await page.route('**/api/clients/list', async (route) => {
      if (latestIdentity) stored.name = 'Latest server name';
      await route.fulfill({ json: { status: true, platform: 'windows', named_certs: [stored] } });
    });
    await page.route('**/api/clients/update', async (route) => {
      const payload = route.request().postDataJSON() as Record<string, unknown>;
      updates.push(payload);
      await updatePending;
      Object.assign(stored, payload);
      await route.fulfill({ json: { status: true } });
    });

    await page.goto('/v2/devices');
    await page.getByRole('button', { name: 'Edit device' }).click();
    const access = page.getByRole('checkbox', { name: 'View stream', exact: true });
    await expect(access).toBeChecked();
    await access.uncheck();
    const save = page.getByRole('button', { name: 'Save device', exact: true });
    const cancel = page.getByRole('button', { name: 'Cancel', exact: true });
    // Check the actual viewport before Save can auto-scroll itself into view.
    await expect(save).toBeInViewport({ ratio: 1 });
    await expect(cancel).toBeInViewport({ ratio: 1 });
    expect(updates).toHaveLength(0);
    await cancel.click();
    await expect(access).toBeChecked();
    expect(updates).toHaveLength(0);
    await access.uncheck();
    await expect(save).toBeInViewport({ ratio: 1 });

    latestIdentity = true;
    try {
      await save.click();
      await expect.poll(() => updates.length).toBe(1);
      await expect(page.getByRole('button', { name: 'Saving', exact: true })).toBeDisabled();
      await expect(cancel).toBeDisabled();
    } finally {
      releaseUpdate();
    }
    await expect(save).toBeEnabled();
    expect(updates[0]).toMatchObject({
      uuid: 'alpha',
      name: 'Latest server name',
      // Only the View stream bit changes; all other permissions remain set.
      perm: 0x051f1f00,
      enable_legacy_ordering: true,
      display_mode: '2560x1440x120',
      hdr_profile: 'living-room.icc',
      prefer_10bit_sdr: true,
      config_overrides: { bitrate: '25000' },
      allow_client_commands: false,
      do: [{ cmd: 'keep-do', elevated: true }],
      undo: [{ cmd: 'keep-undo', elevated: false }],
    });
    await page.reload();
    await page.getByRole('button', { name: 'Edit device' }).click();
    await expect(page.getByRole('checkbox', { name: 'View stream', exact: true })).not.toBeChecked();
    await expect(page.getByRole('checkbox', { name: 'Launch applications', exact: true })).toBeChecked();
    await expect(page.locator('#client-alpha-name')).toHaveValue('Latest server name');
    await expect(page.locator('#client-alpha-display-mode')).toHaveValue('2560x1440x120');
    await expect(page.locator('#client-alpha-commands-allow')).not.toBeChecked();
    // Disabled command policy hides its rows; reveal the retained draft without saving.
    await page.locator('#client-alpha-commands-allow').check();
    await expect(page.locator('#client-alpha-commands-do-0')).toHaveValue('keep-do');
    await expect(page.locator('#client-alpha-commands-undo-0')).toHaveValue('keep-undo');
    expect(updates).toHaveLength(1);
    expect(stored).toMatchObject({
      perm: 0x051f1f00,
      enable_legacy_ordering: true,
      allow_client_commands: false,
      do: [{ cmd: 'keep-do', elevated: true }],
      undo: [{ cmd: 'keep-undo', elevated: false }],
    });
  });
}
