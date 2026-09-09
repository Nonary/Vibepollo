import { expect, test, type Page } from '@playwright/test';

const FIXTURE_NOW = 1_788_955_200;

function makeSummary(uuid: string, start: number, end: number, index: number) {
  return {
    uuid,
    protocol: 'rtsp',
    client_name: 'Moonlight',
    device_name: index === 99 ? 'Bedroom' : 'Living room',
    app_name: index === 98 ? 'Doom' : 'Portal',
    codec: 'h264',
    width: 1920,
    height: 1080,
    target_fps: 60,
    encoder_bitrate_kbps: 20_000,
    requested_bitrate_kbps: 20_000,
    audio_channels: 2,
    hdr: false,
    yuv444: false,
    start_time_unix: start,
    end_time_unix: end,
    duration_seconds: end - start,
  };
}

function makeDetail(summary: ReturnType<typeof makeSummary>) {
  const samples = [0, 30, 60, 90].map((offset, index) => ({
    session_uuid: summary.uuid,
    timestamp_unix: summary.start_time_unix + offset,
    bytes_sent_total: index * 1_000_000,
    packets_sent_video: index * 1000,
    frames_sent: index * 60,
    last_frame_index: index * 60,
    video_dropped: index,
    audio_dropped: index,
    client_reported_losses: index,
    idr_requests: index,
    ref_invalidations: index,
    encode_latency_ms: 4 + index,
    actual_fps: 60,
    actual_bitrate_kbps: 20_000,
    frame_interval_jitter_ms: 1,
    host_cpu_percent: 20 + index,
    host_gpu_percent: 30 + index,
    host_gpu_encoder_percent: 40 + index,
    host_ram_percent: 50 + index,
    host_vram_percent: 60 + index,
    host_net_rx_bps: 2_000_000 + index * 100_000,
    host_net_tx_bps: 4_000_000 + index * 100_000,
  }));
  return {
    ...summary,
    samples,
    events: [
      {
        session_uuid: summary.uuid,
        timestamp_unix: summary.start_time_unix + 45,
        event_type: 'stall',
        payload: 'fixture event',
      },
    ],
    total_samples: samples.length,
    total_events: 1,
    samples_truncated: false,
    events_truncated: false,
  };
}

async function installStatsFixture(page: Page) {
  const all = [
    makeSummary('session-2', FIXTURE_NOW - 120, FIXTURE_NOW - 60, 2),
    makeSummary('session-1', FIXTURE_NOW - 240, FIXTURE_NOW - 180, 1),
    ...Array.from({ length: 8 }, (_, index) => {
      const value = 3 + index;
      const offset = 600 + value * 200;
      return makeSummary(
        `session-${value}`,
        FIXTURE_NOW - offset,
        FIXTURE_NOW - offset + 60,
        value,
      );
    }),
    makeSummary('session-99', FIXTURE_NOW - 2_400, FIXTURE_NOW - 2_340, 99),
    makeSummary('session-98', FIXTURE_NOW - 2_600, FIXTURE_NOW - 2_540, 98),
    makeSummary('session-100', FIXTURE_NOW - 2_800, FIXTURE_NOW - 2_740, 100),
    makeSummary('session-101', FIXTURE_NOW - 3_000, FIXTURE_NOW - 2_940, 101),
  ];
  const deleted = new Set<string>();
  const requests: string[] = [];
  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const url = new URL(request.url());
    const path = url.pathname;
    // The glob also matches Vite's /src/api/* module URLs. Let those modules
    // load or the app remains on the boot screen before StatsView mounts.
    if (!path.startsWith('/api/')) {
      await route.continue();
      return;
    }
    requests.push(`${request.method()} ${path}${url.search}`);
    let body: unknown = { status: true };
    if (path === '/api/auth/status') {
      body = { authenticated: true, login_required: false, credentials_configured: true };
    } else if (path === '/api/configLocale') {
      body = { locale: 'en' };
    } else if (path === '/api/csrf-token') {
      body = { csrf_token: 'fixture-token' };
    } else if (path === '/api/config') {
      body = {
        status: true,
        realtime_stats_enabled: true,
        realtime_stats_poll_interval_ms: 10_000,
        realtime_stats_pause_when_hidden: true,
        realtime_stats_show_active_sessions: false,
        realtime_stats_show_host_stats: false,
        realtime_stats_show_host_charts: false,
        realtime_stats_show_session_history: true,
      };
    } else if (path === '/api/metadata') {
      body = { platform: 'linux', version: 'fixture', capture_status: {} };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false };
    } else if (path === '/api/rtsp/sessions' || path === '/api/webrtc/sessions') {
      body = { sessions: [] };
    } else if (path === '/api/host/stats') {
      body = {
        cpu_percent: 10,
        cpu_temp_c: 40,
        ram_used_bytes: 1,
        ram_total_bytes: 2,
        ram_percent: 50,
        gpu_percent: 20,
        gpu_encoder_percent: 30,
        gpu_temp_c: 45,
        vram_used_bytes: 1,
        vram_total_bytes: 2,
        vram_percent: 50,
        net_rx_bps: 2_000_000,
        net_tx_bps: 4_000_000,
      };
    } else if (path === '/api/host/info') {
      body = { cpu_model: 'Fixture CPU', gpu_model: 'Fixture GPU' };
    } else if (path === '/api/history/sessions' && request.method() === 'GET') {
      const offset = Number(url.searchParams.get('offset') ?? 0);
      const limit = Number(url.searchParams.get('limit') ?? 12);
      const page = all
        .filter((session) => !deleted.has(session.uuid))
        .slice(offset, offset + limit);
      body = { sessions: page };
    } else if (path.startsWith('/api/history/sessions/') && request.method() === 'GET') {
      const uuid = decodeURIComponent(path.split('/').at(-1) ?? '');
      const session = all.find((candidate) => candidate.uuid === uuid);
      if (!session) {
        await route.fulfill({ status: 404, json: { status: false } });
        return;
      }
      body = makeDetail(session);
    } else if (path.startsWith('/api/history/sessions/') && request.method() === 'DELETE') {
      const uuid = decodeURIComponent(path.split('/').at(-1) ?? '');
      deleted.add(uuid);
      body = { status: 'ok', uuid };
    }
    await route.fulfill({ json: body });
  });
  return { requests };
}

test('history pagination, grouped details, full export, deletion, and chart zoom remain usable', async ({
  page,
}, testInfo) => {
  const fixture = await installStatsFixture(page);
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/stats');

  await expect(page.getByRole('heading', { name: 'Session History' })).toBeVisible();
  await expect(page.getByText('Grouped Session (2 streams)', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: /Grouped Session \(2 streams\)/ }).click();
  const detailDialog = page.locator('dialog.stats-detail-dialog[open]');
  await expect(detailDialog.getByRole('button', { name: 'Export JSON' })).toBeVisible();
  await expect(page.getByText('fixture event')).toHaveCount(2);

  await page.getByRole('button', { name: 'Open chart in larger view' }).first().click();
  const chartDialog = page.locator('dialog.metric-chart__dialog[open]');
  await expect(chartDialog).toBeVisible();
  await chartDialog.locator('circle').first().focus();
  await expect(chartDialog.locator('.metric-chart__inspection')).toContainText(':');
  await page.screenshot({
    path: testInfo.outputPath('stats-chart-expanded-1440.png'),
    fullPage: true,
  });
  await chartDialog.getByRole('button', { name: 'Zoom in' }).click();
  await chartDialog.getByRole('button', { name: 'Close' }).click();

  const downloadPromise = page.waitForEvent('download');
  await detailDialog.getByRole('button', { name: 'Export JSON' }).click();
  const download = await downloadPromise;
  expect(download.suggestedFilename()).toMatch(/vibepollo-session-.*\.json/);
  await page.screenshot({ path: testInfo.outputPath('stats-group-1440.png'), fullPage: true });

  await detailDialog.getByRole('button', { name: 'Close' }).click();
  await page.getByRole('button', { name: 'Next page' }).click();
  await expect(page.getByText('2 / 2', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await expect(page.getByText('2 / 2', { exact: true })).toBeVisible();
  expect(fixture.requests.some((request) => request.includes('offset=12'))).toBe(true);

  await page.setViewportSize({ width: 390, height: 900 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  await page.screenshot({ path: testInfo.outputPath('stats-page-390.png'), fullPage: true });

  await page
    .getByRole('button', { name: /Portal|Moonlight/ })
    .last()
    .click();
  const firstDeleteDetail = page.locator('dialog.stats-detail-dialog[open]');
  await firstDeleteDetail.locator('circle').first().focus();
  await expect(firstDeleteDetail.locator('.metric-chart__inspection')).toContainText(':');
  await page.screenshot({ path: testInfo.outputPath('stats-detail-390.png'), fullPage: true });
  await firstDeleteDetail.getByRole('button', { name: 'Delete' }).click();
  await page.locator('dialog.vs-dialog[open]').getByRole('button', { name: 'Delete' }).click();
  await expect(page.getByText('2 / 2', { exact: true })).toBeVisible();

  await page
    .getByRole('button', { name: /Portal|Moonlight/ })
    .last()
    .click();
  const secondDeleteDetail = page.locator('dialog.stats-detail-dialog[open]');
  await secondDeleteDetail.getByRole('button', { name: 'Delete' }).click();
  const secondDeleteConfirm = page.locator('dialog.vs-dialog[open]');
  await expect(secondDeleteConfirm).toContainText('Permanently delete this session');
  await secondDeleteConfirm.getByRole('button', { name: 'Delete' }).click();
  await expect(page.getByText('1 / 1', { exact: true })).toBeVisible();
});
