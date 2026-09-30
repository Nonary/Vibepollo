import { expect, test, type Locator, type Page } from '@playwright/test';

type Layout = { version: number; placements: Record<string, Record<string, unknown>> };
const peerPlacement = { anchor_kind: 'physical', anchor_id: 'primary', edge: 'left', alignment: 'end', gap_px: 16, primary: false };
const manualPlacement = { anchor_kind: 'physical', anchor_id: 'primary', edge: 'below', alignment: 'end', gap_px: 24, primary: false, preserve: true };

async function openEditor(page: Page, placements: Layout['placements'] = { peer: peerPlacement }) {
  const clients = [{ uuid: 'tablet', name: 'Tablet', enabled: true }, { uuid: 'peer', name: 'Peer', enabled: true }];
  let layout: Layout = { version: 1, placements: structuredClone(placements) };
  const initial = structuredClone(layout);
  const saved: Layout[] = [];
  const response = () => ({ status: true, layout, clients, nodes: [{ id: 'primary', label: 'Primary', kind: 'physical', active: true, primary: true, desired_position: { x: 0, y: 0 }, mode: { width: 3840, height: 2160, refresh_hz: 60 } }] });
  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
    if (!path.startsWith('/api/')) { await route.continue(); return; }
    let body: unknown = { status: true };
    if (path === '/api/auth/status') body = { authenticated: true, login_required: false, credentials_configured: true };
    else if (path === '/api/configLocale') body = { locale: 'en' };
    else if (path === '/api/csrf-token') body = { csrf_token: 'fixture-token' };
    else if (path === '/api/config') body = { status: true, virtual_display_mode: 'per_client', virtual_display_layout: 'extended', dd_configuration_option: 'verify_only' };
    else if (path === '/api/metadata') body = { status: true, platform: 'windows', version: 'fixture' };
    else if (path === '/api/session/status') body = { status: true, activeSessions: 0, appRunning: false };
    else if (path === '/api/clients/list') body = { status: true, platform: 'windows', named_certs: clients };
    else if (path === '/api/clients/hdr-profiles') body = { status: true, profiles: [] };
    else if (path === '/api/display-devices') body = [{ device_id: 'primary', friendly_name: 'Primary' }];
    else if (path === '/api/clients/display-layout') {
      if (request.method() === 'PUT') {
        layout = request.postDataJSON();
        saved.push(structuredClone(layout));
      }
      body = response();
    }
    await route.fulfill({ json: body });
  });
  await page.goto('/v2/devices');
  await page.getByText('Arrange client displays', { exact: true }).click();
  const editor = page.locator('.devices-layout .topology-editor');
  await expect(editor.getByRole('button', { name: /Client · Tablet/ })).toBeVisible();
  return { editor, initial, saved };
}

async function saveLayout(editor: Locator, saved: Layout[], count: number) {
  await editor.getByRole('button', { name: 'Save layout', exact: true }).click();
  await expect.poll(() => saved.length).toBe(count);
  await expect(editor.getByRole('button', { name: 'Save layout', exact: true })).toBeEnabled();
}

test('extended client arrangement saves directions and manual state without changing a peer', async ({ page }) => {
  const { editor, saved } = await openEditor(page);
  await editor.getByRole('combobox', { name: 'Client display', exact: true }).selectOption('tablet');
  await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue('automatic');
  await saveLayout(editor, saved, 1);
  expect(saved[0].placements.tablet).toBeUndefined();
  await editor.getByRole('combobox', { name: 'Arrangement', exact: true }).selectOption('below');
  await editor.getByRole('spinbutton', { name: 'Gap (px)', exact: true }).fill('24');
  await saveLayout(editor, saved, 2);
  expect(saved[1].placements.tablet).toMatchObject({ edge: 'below', alignment: 'center', gap_px: 24, preserve: false });
  expect(saved[1].placements.peer).toEqual(saved[0].placements.peer);
  await editor.getByRole('combobox', { name: 'Arrangement', exact: true }).selectOption('preserve');
  await saveLayout(editor, saved, 3);
  expect(saved[2].placements.tablet).toMatchObject({ edge: 'below', gap_px: 24, preserve: true });
  expect(saved[2].placements.peer).toEqual(saved[0].placements.peer);
});

for (const arrangement of ['preserve', 'below', 'automatic'] as const) {
  test(`clicking client rectangles preserves the saved ${arrangement} arrangement`, async ({ page }) => {
    const placements = { peer: peerPlacement } as Layout['placements'];
    if (arrangement !== 'automatic') placements.tablet = { ...manualPlacement, preserve: arrangement === 'preserve' };
    const { editor, initial, saved } = await openEditor(page, placements);
    await editor.getByRole('button', { name: /Client · Tablet/ }).click();
    await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue(arrangement);
    await editor.getByRole('button', { name: /Client · Peer/ }).click();
    await expect(editor.getByRole('combobox', { name: 'Client display', exact: true })).toHaveValue('peer');
    await saveLayout(editor, saved, 1);
    expect(saved[0]).toEqual(initial);
  });
}

test('a real drag outside the client rectangle changes only its placement', async ({ page }) => {
  const { editor, initial, saved } = await openEditor(page, { tablet: manualPlacement, peer: peerPlacement });
  const tablet = await editor.getByRole('button', { name: /Client · Tablet/ }).boundingBox();
  const physical = await editor.getByRole('button', { name: /Physical · Primary/ }).boundingBox();
  expect(tablet).not.toBeNull(); expect(physical).not.toBeNull();
  await page.mouse.move(tablet!.x + tablet!.width / 2, tablet!.y + tablet!.height / 2);
  await page.mouse.down();
  await page.mouse.move(physical!.x - 10, physical!.y + 5, { steps: 5 });
  await page.mouse.up();
  await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue('left');
  await saveLayout(editor, saved, 1);
  expect(saved[0].placements.tablet).toMatchObject({ anchor_kind: 'physical', anchor_id: 'primary', edge: 'left', alignment: 'center', gap_px: 0, preserve: false });
  expect(saved[0].placements.peer).toEqual(initial.placements.peer);
});

test('keyboard placement belongs to the focused client and ignores physical rectangles', async ({ page }) => {
  const { editor, initial, saved } = await openEditor(page, { tablet: manualPlacement, peer: peerPlacement });
  await editor.getByRole('combobox', { name: 'Client display', exact: true }).selectOption('peer');
  await editor.getByRole('button', { name: /Client · Tablet/ }).focus();
  await page.keyboard.press('ArrowUp');
  await expect(editor.getByRole('combobox', { name: 'Client display', exact: true })).toHaveValue('tablet');
  await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue('above');
  await editor.getByRole('button', { name: /Physical · Primary/ }).focus();
  await page.keyboard.press('ArrowRight');
  await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue('above');
  await saveLayout(editor, saved, 1);
  expect(saved[0].placements.tablet).toMatchObject({ edge: 'above', gap_px: 24, alignment: 'end', preserve: false });
  expect(saved[0].placements.peer).toEqual(initial.placements.peer);
});

test.describe('touch gestures', () => {
  test.use({ hasTouch: true });

  test('cancelled touch movement keeps the rule and releases the gesture for selection', async ({ page, context }) => {
    const { editor, initial, saved } = await openEditor(page, { tablet: manualPlacement, peer: peerPlacement });
    const tablet = editor.getByRole('button', { name: /Client · Tablet/ });
    const box = await tablet.boundingBox();
    expect(box).not.toBeNull();
    const start = { x: box!.x + box!.width / 2, y: box!.y + box!.height / 2, id: 1 };
    const session = await context.newCDPSession(page);
    await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [start] });
    await session.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ ...start, x: start.x - 40 }] });
    await session.send('Input.dispatchTouchEvent', { type: 'touchCancel', touchPoints: [] });
    await tablet.tap();
    await expect(editor.getByRole('combobox', { name: 'Arrangement', exact: true })).toHaveValue('preserve');
    await saveLayout(editor, saved, 1);
    expect(saved[0]).toEqual(initial);
    await session.detach();
  });
});
