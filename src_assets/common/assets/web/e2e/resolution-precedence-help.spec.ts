import { test, expect, type Locator, type Page } from '@playwright/test';

async function host(page: Page, locale: string) {
  const writes: string[] = [];
  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (!path.startsWith('/api/')) return route.continue();
    if (request.method() !== 'GET') writes.push(path);
    let body: unknown = { status: true };
    if (path === '/api/auth/status') {
      body = { authenticated: true, login_required: false, credentials_configured: true };
    } else if (path === '/api/configLocale') {
      body = { locale };
    } else if (path === '/api/csrf-token') {
      body = { csrf_token: 'resolution-help-test' };
    } else if (path === '/api/metadata') {
      body = { platform: 'windows', windows_build_number: 26100, version: '2.0.1' };
    } else if (path === '/api/config') {
      body = {
        status: true,
        locale,
        dd_configuration_option: 'verify_only',
        dd_resolution_option: 'manual',
        dd_manual_resolution: '1280x720',
        dd_refresh_rate_option: 'manual',
        dd_manual_refresh_rate: '75',
      };
    } else if (path === '/api/apps') {
      body = {
        apps: [
          {
            uuid: '11111111-1111-4111-8111-111111111111',
            name: 'Resolution help app',
            'config-overrides': {
              dd_resolution_option: 'manual',
              dd_manual_resolution: '1920x1080',
              dd_refresh_rate_option: 'manual',
              dd_manual_refresh_rate: '90',
            },
          },
        ],
      };
    } else if (path === '/api/display-devices') {
      body = [];
    } else if (path === '/api/session/status') {
      body = { activeSessions: 0, appRunning: false };
    }
    await route.fulfill({ json: body });
  });
  return writes;
}

async function expectLayeringHelp(help: Locator) {
  await expect(help).toContainText(
    'For a new app launch with no app or stream already running, matching configuration values take priority in this order: Client, Application, Global',
  );
  await expect(help).toContainText(
    "Joining an active shared session or reconnecting to an existing app, including a paused app, may keep the host's existing settings",
  );
}

async function expectResolutionHelp(help: Locator) {
  await expectLayeringHelp(help);
  await expect(help).toContainText(
    "A client's Display mode override and launch resolution scaling take priority",
  );
}

async function expectRefreshHelp(help: Locator) {
  await expectLayeringHelp(help);
  await expect(help).toContainText("A client's Display mode override takes priority");
}

for (const locale of ['en', 'en_US', 'en_GB']) {
  test(`legacy application explains manual display exceptions in ${locale}`, async ({
    page,
  }, testInfo) => {
    const writes = await host(page, locale);
    await page.goto('/applications');
    await page.getByRole('button', { name: /Resolution help app/ }).click();
    await expect(page.locator('#configure_display_resolution input')).toHaveValue('1920x1080');
    await expect(page.locator('#configure_display_refresh_rate input')).toHaveValue('90');
    await page.locator('#configure_display_resolution input').scrollIntoViewIfNeeded();
    await page.screenshot({ path: testInfo.outputPath('manual-display-help.png') });
    await expectResolutionHelp(
      page.getByText(/^Uses the manual resolution instead of resolution remapping rules/),
    );
    await expectRefreshHelp(
      page.getByText(/^Uses the manual refresh rate instead of refresh remapping rules/),
    );
    await expect(
      page.getByText(
        /The host will always be set to this resolution|forces the host to this refresh for all streams/,
      ),
    ).toHaveCount(0);
    expect(writes).toEqual([]);
  });

  test(`legacy global settings explain manual display exceptions in ${locale}`, async ({
    page,
  }, testInfo) => {
    const writes = await host(page, locale);
    await page.goto('/settings?sec=av&jump=dd_manual_refresh_rate');
    await expect(page.locator('#dd_manual_resolution input')).toHaveValue('1280x720');
    await expect(page.locator('#dd_manual_refresh_rate input')).toHaveValue('75');
    await page.locator('#dd_manual_refresh_rate input').scrollIntoViewIfNeeded();
    await page.screenshot({ path: testInfo.outputPath('manual-display-help.png') });
    await expectResolutionHelp(
      page.getByText(/^Uses the manual resolution instead of resolution remapping rules/),
    );
    await expectRefreshHelp(
      page.getByText(/^Uses the manual refresh rate instead of refresh remapping rules/),
    );
    const notice = page.getByText(
      /^Manual values take priority over remapping for that value/,
    );
    await expect(notice).toContainText('Values set to Match the client keep their rules');
    await expect(notice).toContainText(
      'that override bypasses all remapping and takes priority over manual values',
    );
    await expect(
      page.getByText(
        /The host will always be set to this resolution|forces the host to this refresh for all streams/,
      ),
    ).toHaveCount(0);
    expect(writes).toEqual([]);
  });

  test(`current Settings explains manual display exceptions in ${locale}`, async ({
    page,
  }, testInfo) => {
    const writes = await host(page, locale);
    await page.goto('/v2/settings?category=display');
    await expect(page.locator('#setting-dd_manual_resolution')).toHaveValue('1280x720');
    await expect(page.locator('#setting-dd_manual_refresh_rate')).toHaveValue('75');
    await page
      .locator('#setting-dd_manual_resolution')
      .evaluate((element) => element.scrollIntoView({ block: 'center' }));
    await page.screenshot({ path: testInfo.outputPath('manual-display-help.png') });
    await expectResolutionHelp(page.locator('#setting-dd_manual_resolution-description'));
    await expectRefreshHelp(page.locator('#setting-dd_manual_refresh_rate-description'));
    const remappingHelp = page.getByText(
      /^A fixed resolution or refresh rate takes priority over remapping rules/,
    );
    await expectLayeringHelp(remappingHelp);
    await expect(remappingHelp).toContainText(
      "A client's Display mode override takes priority over manual values",
    );
    await expect(remappingHelp).toContainText(
      "unless a client's Display mode override is active; that override bypasses all remapping",
    );
    expect(writes).toEqual([]);
  });
}
