import { expect, test, type Page } from '@playwright/test';
import { readFileSync } from 'node:fs';
import ts from 'typescript';

// Run the actual legacy capture module in a browser, including RAF coalescing.
const legacySource = readFileSync(
  new URL('../../web-legacy/utils/webrtc/input.ts', import.meta.url),
  'utf8',
);
const legacyCode = ts.transpileModule(legacySource, {
  compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
}).outputText;
const clientCode = ts.transpileModule(
  readFileSync(new URL('../../web-legacy/utils/webrtc/client.ts', import.meta.url), 'utf8'),
  { compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 } },
).outputText;

test.use({ hasTouch: true });

async function capture(page: Page) {
  await page.setContent(
    '<div id="capture" tabindex="0" style="width:400px;height:200px;touch-action:pan-y"></div>',
  );
  await page.addScriptTag({
    content: `(() => { const exports = {}; ${legacyCode}; window.legacyCapture = exports.attachInputCapture; })();`,
  });
  await page.evaluate(() => {
    const target = document.querySelector('#capture') as HTMLElement;
    const video = document.createElement('video');
    Object.defineProperties(video, { videoWidth: { value: 1920 }, videoHeight: { value: 1080 } });
    const messages: Record<string, unknown>[] = [];
    Object.assign(window, { touchMessages: messages });
    (window as any).detachTouch = (window as any).legacyCapture(
      target,
      (payload: string | ArrayBuffer) => {
        if (typeof payload === 'string') messages.push(JSON.parse(payload));
        else {
          const value = new DataView(payload);
          messages.push({
            type: 'mouse_move',
            x: value.getUint16(3, true) / 65535,
            y: value.getUint16(5, true) / 65535,
          });
        }
      },
      { video, gamepad: false },
    );
  });
}

async function pointer(page: Page, type: string, options: Record<string, unknown> = {}) {
  return page.locator('#capture').evaluate(
    (target, { type, options }) => {
      const rect = target.getBoundingClientRect();
      return target.dispatchEvent(
        new PointerEvent(type, {
          bubbles: true,
          cancelable: true,
          pointerId: 1,
          pointerType: 'touch',
          isPrimary: true,
          button: 0,
          buttons: type === 'pointerup' ? 0 : 1,
          width: 24,
          height: 24,
          clientX: rect.left + 200,
          clientY: rect.top + 100,
          ...options,
        }),
      );
    },
    { type, options },
  );
}

async function messages(page: Page) {
  return page.evaluate(() => (window as any).touchMessages as Record<string, unknown>[]);
}

async function captureWithKeyboard(page: Page) {
  // A localhost document supplies the secure context required by Keyboard Lock.
  await page.route('**/legacy-touch-focus', (route) =>
    route.fulfill({
      contentType: 'text/html',
      body: '<!doctype html><title>Input capture</title>',
    }),
  );
  await page.goto('/legacy-touch-focus');
  await capture(page);
  await page.evaluate(() => {
    const other = document.createElement('button');
    other.id = 'other';
    other.textContent = 'Other control';
    document.body.append(other);
    const target = document.querySelector('#capture') as HTMLElement;
    const state = { focusCalls: 0, locks: 0, unlocks: 0, trustedTouches: 0 };
    Object.assign(window, { keyboardCaptureState: state });
    const focus = target.focus.bind(target);
    target.focus = (options) => {
      state.focusCalls += 1;
      focus(options);
    };
    target.addEventListener('pointerdown', (event) => {
      if (event.pointerType === 'touch' && event.isTrusted) state.trustedTouches += 1;
    });
    // Mock only the browser permission boundary, retaining production lock lifecycle.
    Object.defineProperty(navigator, 'keyboard', {
      configurable: true,
      value: {
        async lock() {
          state.locks += 1;
        },
        unlock() {
          state.unlocks += 1;
        },
      },
    });
    other.focus();
  });
}

async function keyboardState(page: Page) {
  return page.evaluate(() => (window as any).keyboardCaptureState);
}

test('legacy trusted content touch acquires focus and forwards following hardware keys', async ({
  page,
}) => {
  await captureWithKeyboard(page);
  expect(await page.evaluate(() => window.isSecureContext)).toBe(true);
  expect(await page.evaluate(() => document.fullscreenElement)).toBe(null);
  await expect(page.locator('#other')).toBeFocused();
  const rect = await page.locator('#capture').boundingBox();
  await page.touchscreen.tap(rect!.x + 200, rect!.y + 100);
  await expect(page.locator('#capture')).toBeFocused();
  expect(await keyboardState(page)).toMatchObject({ focusCalls: 1, locks: 1, trustedTouches: 1 });
  await page.keyboard.press('a');
  expect((await messages(page)).map((message) => message.type)).toEqual([
    'mouse_down',
    'mouse_up',
    'key_down',
    'key_up',
  ]);
  expect((await messages(page)).slice(2)).toMatchObject([
    { type: 'key_down', key: 'a', code: 'KeyA' },
    { type: 'key_up', key: 'a', code: 'KeyA' },
  ]);
  await page.locator('#other').focus();
  expect(await keyboardState(page)).toMatchObject({ locks: 1, unlocks: 1 });
  await page.keyboard.press('b');
  expect(await messages(page)).toHaveLength(4);
});

test('legacy trusted letterbox touch leaves other controls and keyboard capture alone', async ({
  page,
}) => {
  await captureWithKeyboard(page);
  const rect = await page.locator('#capture').boundingBox();
  await page.touchscreen.tap(rect!.x + 1, rect!.y + 100);
  await expect(page.locator('#other')).toBeFocused();
  expect(await keyboardState(page)).toMatchObject({ focusCalls: 0, locks: 0, trustedTouches: 1 });
  await page.keyboard.press('a');
  expect(await messages(page)).toEqual([]);
});

test('legacy secondary and competing touches cannot acquire capture focus or keyboard lock', async ({
  page,
}) => {
  await captureWithKeyboard(page);
  await pointer(page, 'pointerdown', { pointerId: 2, isPrimary: false });
  await pointer(page, 'pointerup', { pointerId: 2, isPrimary: false });
  await expect(page.locator('#other')).toBeFocused();
  expect(await keyboardState(page)).toMatchObject({ focusCalls: 0, locks: 0 });
  await page.keyboard.press('a');
  expect(await messages(page)).toEqual([]);

  await pointer(page, 'pointerdown');
  await expect(page.locator('#capture')).toBeFocused();
  expect(await keyboardState(page)).toMatchObject({ focusCalls: 1, locks: 1 });
  for (const isPrimary of [false, true]) {
    await pointer(page, 'pointerdown', { pointerId: 2, isPrimary });
    await pointer(page, 'pointerup', { pointerId: 2, isPrimary });
  }
  expect(await keyboardState(page)).toMatchObject({ focusCalls: 1, locks: 1 });
  expect(await messages(page)).toEqual([]);
  await pointer(page, 'pointerup');
  expect((await messages(page)).map((message) => message.type)).toEqual(['mouse_down', 'mouse_up']);
});

test('legacy trusted touch tap sends one click and suppresses page gestures', async ({ page }) => {
  await capture(page);
  await expect(page.locator('#capture')).toHaveCSS('touch-action', 'none');
  const rect = await page.locator('#capture').boundingBox();
  await page.touchscreen.tap(rect!.x + 200, rect!.y + 100);
  expect(await messages(page)).toMatchObject([
    { type: 'mouse_down', button: 0, x: 0.5, y: 0.5 },
    { type: 'mouse_up', button: 0, x: 0.5, y: 0.5 },
  ]);
  expect(await messages(page)).toHaveLength(2);
});

test('legacy tap freezes jitter and ignores touches in letterboxing', async ({ page }) => {
  await capture(page);
  expect(await pointer(page, 'pointerdown')).toBe(false);
  await pointer(page, 'pointermove', { clientX: 213, clientY: 113 });
  await pointer(page, 'pointerup', { clientX: 219, clientY: 117 });
  expect(await messages(page)).toMatchObject([
    { type: 'mouse_down', x: 0.5, y: 0.5 },
    { type: 'mouse_up', x: 0.5, y: 0.5 },
  ]);
  await pointer(page, 'pointerdown', { clientX: 9 });
  await pointer(page, 'pointerup', { clientX: 9 });
  expect(await messages(page)).toHaveLength(2);
});

test('legacy drag clamps release and secondary fingers cannot move or release it', async ({
  page,
}) => {
  await capture(page);
  await pointer(page, 'pointerdown');
  await pointer(page, 'pointermove', { clientX: 300, clientY: 150 });
  await page.evaluate(() => new Promise(requestAnimationFrame));
  const before = await messages(page);
  expect(before.map((m) => m.type)).toEqual(['mouse_down', 'mouse_move']);
  for (const type of [
    'pointerdown',
    'pointermove',
    'pointerup',
    'pointercancel',
    'lostpointercapture',
  ]) {
    await pointer(page, type, { pointerId: 2, isPrimary: false, clientX: 50, clientY: 50 });
  }
  expect(await messages(page)).toEqual(before);
  await pointer(page, 'pointerup', { clientX: 800, clientY: 800 });
  expect((await messages(page)).at(-1)).toMatchObject({ type: 'mouse_up', x: 1, y: 1 });
});

for (const end of ['pointercancel', 'lostpointercapture', 'blur', 'detach']) {
  test(`legacy ${end} cancels taps and releases dragging once`, async ({ page }) => {
    await capture(page);
    const stop = async () => {
      if (end === 'detach') await page.evaluate(() => (window as any).detachTouch());
      else if (end === 'blur') await page.locator('#capture').dispatchEvent('blur');
      else await pointer(page, end);
    };
    await pointer(page, 'pointerdown');
    await stop();
    await pointer(page, 'pointerup');
    expect(await messages(page)).toEqual([]);
    if (end === 'detach') await capture(page);
    await pointer(page, 'pointerdown');
    await pointer(page, 'pointermove', { clientX: 300 });
    await stop();
    await pointer(page, 'pointerup');
    await page.evaluate(() => new Promise(requestAnimationFrame));
    expect((await messages(page)).map((m) => m.type)).toEqual([
      'mouse_down',
      'mouse_move',
      'mouse_up',
    ]);
    if (end === 'detach') await expect(page.locator('#capture')).toHaveCSS('touch-action', 'pan-y');
  });
}

test('legacy mouse input keeps its immediate buttons and coalesced movement', async ({ page }) => {
  await capture(page);
  const bounds = await page.locator('#capture').boundingBox();
  await page.mouse.move(bounds!.x + 200, bounds!.y + 100);
  await page.evaluate(() => new Promise(requestAnimationFrame));
  await page.evaluate(() => {
    (window as any).touchMessages.length = 0;
  });
  await page.mouse.down();
  expect((await messages(page)).at(-1)).toMatchObject({ type: 'mouse_down', x: 0.5, y: 0.5 });
  await page.mouse.move(bounds!.x + 300, bounds!.y + 100);
  await page.evaluate(() => new Promise(requestAnimationFrame));
  await page.mouse.up();
  expect((await messages(page)).map((m) => m.type)).toEqual([
    'mouse_down',
    'mouse_move',
    'mouse_up',
  ]);
});

test('legacy flushes queued motion before a tap and keeps the input channel ordered', async ({
  page,
}) => {
  await capture(page);
  // Dispatch in one browser turn so RAF cannot run before the touch starts.
  await page.locator('#capture').evaluate((target) => {
    const bounds = target.getBoundingClientRect();
    for (const [type, pointerType, offset] of [
      ['pointermove', 'mouse', 100],
      ['pointerdown', 'touch', 200],
      ['pointerup', 'touch', 200],
    ] as const) {
      target.dispatchEvent(
        new PointerEvent(type, {
          pointerId: 1,
          pointerType,
          isPrimary: true,
          cancelable: true,
          button: 0,
          clientX: bounds.left + Number(offset),
          clientY: bounds.top + 100,
        }),
      );
    }
  });
  await page.evaluate(() => new Promise(requestAnimationFrame));
  expect((await messages(page)).map((m) => m.type)).toEqual([
    'mouse_move',
    'mouse_down',
    'mouse_up',
  ]);
  expect((await messages(page)).slice(1)).toMatchObject([{ x: 0.5 }, { x: 0.5 }]);

  await page.addScriptTag({
    content: `(() => { const exports = {}; ${clientCode}; window.LegacyClient = exports.WebRtcClient; })();`,
  });
  const channelOptions = await page.evaluate(async () => {
    let options: RTCDataChannelInit | undefined;
    const sdp = 'v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\n';
    class Peer {
      addTransceiver() {
        return { setCodecPreferences() {} };
      }
      createDataChannel(_name: string, init: RTCDataChannelInit) {
        options = init;
        return { readyState: 'open', close() {} };
      }
      async createOffer() {
        return { type: 'offer', sdp };
      }
      async setLocalDescription() {}
      async setRemoteDescription() {}
      close() {}
    }
    Object.assign(window, { RTCPeerConnection: Peer });
    const api = {
      async createSession() {
        return { sessionId: 'touch-test', iceServers: [] };
      },
      subscribeRemoteCandidates() {
        return () => {};
      },
      async sendOffer() {
        return { type: 'answer', sdp };
      },
      async endSession() {},
    };
    const client = new (window as any).LegacyClient(api);
    await client.connect({ encoding: 'h264', hdr: false, bitrateKbps: 10000 });
    await client.disconnect();
    return options;
  });
  expect(channelOptions).toMatchObject({ ordered: true, maxRetransmits: 0 });
});
