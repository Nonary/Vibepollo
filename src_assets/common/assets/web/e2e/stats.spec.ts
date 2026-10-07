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
      ...[
        ['recovery', 'recovered'],
        ['first_drop', 'first loss'],
        ['drop_burst', 'loss burst'],
        ['stream_started', 'stream began'],
        ['stream_ended', 'stream ended'],
      ].map(([event_type, payload], index) => ({
        session_uuid: summary.uuid,
        timestamp_unix: summary.start_time_unix + 46 + index,
        event_type,
        payload,
      })),
    ],
    total_samples: samples.length,
    total_events: 6,
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
  let hostStatsRequestCount = 0;
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
        realtime_stats_show_host_charts: true,
        realtime_stats_show_session_history: true,
      };
    } else if (path === '/api/metadata') {
      body = { platform: 'linux', version: 'fixture', capture_status: {} };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: 0, appRunning: false };
    } else if (path === '/api/rtsp/sessions' || path === '/api/webrtc/sessions') {
      body = { sessions: [] };
    } else if (path === '/api/host/stats') {
      const sampleIndex = hostStatsRequestCount++;
      body = {
        cpu_percent: 10,
        cpu_temp_c: 40,
        ram_used_bytes: 1,
        ram_total_bytes: 2,
        ram_percent: sampleIndex === 1 ? null : 42 + sampleIndex * 6,
        gpu_percent: 20,
        gpu_encoder_percent: 30,
        gpu_temp_c: 45,
        vram_used_bytes: 1,
        vram_total_bytes: 2,
        vram_percent: 64 + sampleIndex * 7,
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
  await expect(page.getByText('Host RAM / VRAM Usage', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await page.screenshot({ path: '/tmp/vibeshine-stats-host-memory-1440.png', fullPage: true });
  await expect(page.getByText('Grouped Session (2 streams)', { exact: true })).toBeVisible();
  await page.getByRole('button', { name: /Grouped Session \(2 streams\)/ }).click();
  const detailDialog = page.locator('dialog.stats-detail-dialog[open]');
  await expect(detailDialog.getByRole('button', { name: 'Export JSON' })).toBeVisible();
  await expect(detailDialog.locator('.event-list').getByText('fixture event')).toHaveCount(2);

  await detailDialog.getByRole('button', { name: 'Open chart in larger view' }).first().click();
  const chartDialog = page.locator('dialog.metric-chart__dialog[open]');
  await expect(chartDialog).toBeVisible();
  await expect(chartDialog.locator('.metric-chart__event')).toHaveCount(12);
  await expect(chartDialog.getByRole('button', { name: /stall/ }).first()).toBeAttached();
  const plot = chartDialog.locator('.metric-chart__dialog-plot svg');
  const readRange = async () => [
    Number(await plot.getAttribute('data-view-start')),
    Number(await plot.getAttribute('data-view-end')),
  ];
  const fullRange = await readRange();
  await page.screenshot({
    path: '/tmp/vibeshine-stats-chart-dense-events-1440.png',
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 1000 });
  await expect(chartDialog.locator('.metric-chart__event')).toHaveCount(12);
  const narrowDialogBox = await chartDialog.boundingBox();
  const narrowToolbar = chartDialog.locator('.metric-chart__zoom-actions');
  const narrowToolbarBox = await narrowToolbar.boundingBox();
  expect(narrowDialogBox).not.toBeNull();
  expect(narrowToolbarBox).not.toBeNull();
  expect(narrowDialogBox!.x).toBeGreaterThanOrEqual(0);
  expect(narrowDialogBox!.x + narrowDialogBox!.width).toBeLessThanOrEqual(390);
  expect(narrowToolbarBox!.x).toBeGreaterThanOrEqual(narrowDialogBox!.x);
  expect(narrowToolbarBox!.x + narrowToolbarBox!.width).toBeLessThanOrEqual(
    narrowDialogBox!.x + narrowDialogBox!.width,
  );
  for (const label of ['Zoom out', 'Zoom in', 'Reset zoom']) {
    await expect(narrowToolbar.getByRole('button', { name: label })).toBeVisible();
  }
  await page.screenshot({
    path: '/tmp/vibeshine-stats-chart-dense-events-390.png',
    fullPage: true,
  });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await chartDialog.locator('.metric-chart__event').first().focus();
  await expect(chartDialog.locator('.metric-chart__event-inspection')).toContainText('stall');
  await chartDialog.locator('circle').first().focus();
  await expect(chartDialog.locator('.metric-chart__inspection')).toContainText(':');
  await page.screenshot({
    path: testInfo.outputPath('stats-chart-expanded-1440.png'),
    fullPage: true,
  });
  await chartDialog.getByRole('button', { name: 'Zoom in' }).click();
  await expect(chartDialog.getByRole('button', { name: 'Reset zoom' })).not.toBeDisabled();
  const afterButtonZoom = await readRange();
  expect(afterButtonZoom[1] - afterButtonZoom[0]).toBeLessThan(fullRange[1] - fullRange[0]);
  const plotBox = await plot.boundingBox();
  if (!plotBox) throw new Error('expanded chart plot has no layout box');
  await page.keyboard.down('Shift');
  await page.mouse.move(plotBox.x + plotBox.width * 0.65, plotBox.y + plotBox.height * 0.5);
  await page.mouse.down();
  await page.mouse.move(plotBox.x + plotBox.width * 0.45, plotBox.y + plotBox.height * 0.5);
  await page.mouse.up();
  await page.keyboard.up('Shift');
  const afterPan = await readRange();
  expect(afterPan).not.toEqual(afterButtonZoom);
  await plot.dispatchEvent('wheel', { deltaY: -120, bubbles: true });
  const afterWheel = await readRange();
  expect(afterWheel[1] - afterWheel[0]).toBeLessThan(afterPan[1] - afterPan[0]);
  await plot.dispatchEvent('pointerdown', {
    pointerId: 101,
    pointerType: 'touch',
    clientX: plotBox.x + plotBox.width * 0.35,
    clientY: plotBox.y + plotBox.height * 0.5,
    bubbles: true,
  });
  await plot.dispatchEvent('pointerdown', {
    pointerId: 102,
    pointerType: 'touch',
    clientX: plotBox.x + plotBox.width * 0.55,
    clientY: plotBox.y + plotBox.height * 0.5,
    bubbles: true,
  });
  await plot.dispatchEvent('pointermove', {
    pointerId: 102,
    pointerType: 'touch',
    clientX: plotBox.x + plotBox.width * 0.7,
    clientY: plotBox.y + plotBox.height * 0.5,
    bubbles: true,
  });
  await plot.dispatchEvent('pointerup', {
    pointerId: 101,
    pointerType: 'touch',
    clientX: plotBox.x + plotBox.width * 0.35,
    clientY: plotBox.y + plotBox.height * 0.5,
    bubbles: true,
  });
  await plot.dispatchEvent('pointerup', {
    pointerId: 102,
    pointerType: 'touch',
    clientX: plotBox.x + plotBox.width * 0.7,
    clientY: plotBox.y + plotBox.height * 0.5,
    bubbles: true,
  });
  const afterPinch = await readRange();
  expect(afterPinch[1] - afterPinch[0]).toBeLessThan(afterWheel[1] - afterWheel[0]);
  await expect(plot).toHaveCSS('touch-action', 'pan-y');
  await page.screenshot({
    path: '/tmp/vibeshine-stats-chart-gestures-1440.png',
    fullPage: true,
  });
  await chartDialog.getByRole('button', { name: 'Reset zoom' }).click();
  expect(await readRange()).toEqual(fullRange);
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
  await page.screenshot({ path: '/tmp/vibeshine-stats-narrow-390.png', fullPage: true });
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
