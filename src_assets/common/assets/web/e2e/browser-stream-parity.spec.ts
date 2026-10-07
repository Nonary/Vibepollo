import { test, expect, type Page } from '@playwright/test';

type SessionPayload = Record<string, unknown>;

interface BrowserStreamFixtureOptions {
  delayTerminateMs?: number;
  failConnection?: boolean;
  includeApp?: boolean;
  rejectFullscreen?: boolean;
  runningSession?: boolean;
}

async function installBrowserStreamFixtures(
  page: Page,
  options: BrowserStreamFixtureOptions = {},
): Promise<SessionPayload[]> {
  const sessions: SessionPayload[] = [];
  let sessionRunning = options.runningSession === true;

  await page.addInitScript(
    ({ rejectFullscreen }) => {
      const fullscreenCalls: string[] = [];
      Object.defineProperty(Element.prototype, 'requestFullscreen', {
        configurable: true,
        value: function requestFullscreen() {
          fullscreenCalls.push((this as HTMLElement).id || this.tagName.toLocaleLowerCase());
          return rejectFullscreen
            ? Promise.reject(new Error('Fullscreen request rejected'))
            : Promise.resolve();
        },
      });
      if (rejectFullscreen) {
        // Chromium exposes prefixed request methods alongside the standard
        // method. Remove every native path so this fixture reaches the
        // component's pseudo-fullscreen fallback deterministically.
        for (const prototype of [Element.prototype, HTMLVideoElement.prototype]) {
          for (const method of ['webkitRequestFullscreen', 'webkitRequestFullScreen']) {
            Object.defineProperty(prototype, method, { configurable: true, value: undefined });
          }
        }
        for (const method of ['webkitEnterFullscreen', 'webkitEnterFullScreen']) {
          Object.defineProperty(HTMLVideoElement.prototype, method, {
            configurable: true,
            value: undefined,
          });
        }
      }
      Object.assign(window, { __browserStreamFullscreenCalls: fullscreenCalls });
    },
    { rejectFullscreen: options.rejectFullscreen === true },
  );
  await page.addInitScript(() => {
    const inputMessages: unknown[] = [];
    const hapticEffects: unknown[] = [];
    const gamepad = {
      axes: [0.5, 0, 0, 0],
      buttons: Array.from({ length: 18 }, (_value, index) => ({
        pressed: index === 0,
        touched: index === 0,
        value: index === 0 ? 1 : 0,
      })),
      connected: true,
      hapticActuators: [
        {
          playEffect: (_kind: string, effect: unknown) => {
            hapticEffects.push(effect);
            return Promise.resolve('complete');
          },
        },
      ],
      // Match Chromium's canonical Xbox mapping. The legacy fallback treats a
      // bare "Wireless Controller" as PlayStation, so keep the vendor token
      // explicit in this Xbox fixture.
      id: 'Xbox 360 Controller (XInput STANDARD GAMEPAD)',
      index: 0,
      mapping: 'standard',
      timestamp: 1,
    };
    Object.defineProperties(window.navigator, {
      getGamepads: { configurable: true, value: () => [gamepad] },
    });
    Object.assign(window, {
      __browserStreamGamepad: gamepad,
      __browserStreamHaptics: hapticEffects,
      __browserStreamInputs: inputMessages,
    });

    class FakeDataChannel {
      readyState = 'connecting';
      onopen: (() => void) | null = null;
      onclose: (() => void) | null = null;
      onerror: (() => void) | null = null;
      onmessage: ((event: { data: string }) => void) | null = null;
      send(payload: string): void {
        try {
          inputMessages.push(JSON.parse(payload));
        } catch {
          // The fixture only needs to observe JSON input messages.
        }
      }
      close(): void {
        this.readyState = 'closed';
        this.onclose?.();
      }
    }

    class FakePeerConnection {
      connectionState = 'new';
      onconnectionstatechange: (() => void) | null = null;
      onicecandidate: ((event: { candidate: null }) => void) | null = null;
      ontrack: ((event: unknown) => void) | null = null;
      private readonly channel = new FakeDataChannel();
      private statsCount = 0;

      addTransceiver(): { setCodecPreferences: () => void } {
        return { setCodecPreferences: () => undefined };
      }

      createDataChannel(): FakeDataChannel {
        Object.assign(window, { __browserStreamInputChannel: this.channel });
        window.setTimeout(() => {
          this.channel.readyState = 'open';
          this.channel.onopen?.();
        }, 0);
        return this.channel;
      }

      async createOffer(): Promise<RTCSessionDescriptionInit> {
        return {
          type: 'offer',
          sdp: 'v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n',
        };
      }

      async setLocalDescription(): Promise<void> {}

      async setRemoteDescription(): Promise<void> {
        window.setTimeout(() => {
          this.connectionState = 'connected';
          this.onconnectionstatechange?.();
        }, 0);
      }

      getReceivers(): [] {
        return [];
      }

      async addIceCandidate(): Promise<void> {}

      async getStats(): Promise<Map<string, Record<string, unknown>>> {
        this.statsCount += 1;
        return new Map([
          [
            'video-inbound',
            {
              id: 'video-inbound',
              type: 'inbound-rtp',
              kind: 'video',
              bytesReceived: this.statsCount * 100_000,
              framesDecoded: this.statsCount * 6,
              framesReceived: this.statsCount * 6,
              framesDropped: 2,
              packetsReceived: this.statsCount * 8,
              jitter: 0.004,
              codecId: 'video-codec',
            },
          ],
          ['video-codec', { id: 'video-codec', type: 'codec', mimeType: 'video/H264' }],
          [
            'candidate-pair',
            {
              id: 'candidate-pair',
              type: 'candidate-pair',
              state: 'succeeded',
              selected: true,
              currentRoundTripTime: 0.024,
              protocol: 'udp',
            },
          ],
        ]);
      }

      close(): void {
        this.connectionState = 'closed';
        this.channel.close();
      }
    }

    class FakeEventSource {
      onerror: (() => void) | null = null;
      addEventListener(): void {}
      close(): void {}
    }

    Object.assign(window, {
      RTCPeerConnection: FakePeerConnection,
      EventSource: FakeEventSource,
    });
  });

  await page.route('**/api/**', async (route) => {
    const request = route.request();
    const path = new URL(request.url()).pathname;
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
      body = { csrf_token: 'browser-stream-test' };
    } else if (path === '/api/apps') {
      body = options.includeApp
        ? { apps: [{ index: 1, name: 'Test Game', uuid: 'browser-stream-test-app' }] }
        : { apps: [] };
    } else if (path === '/api/session/status') {
      body = { status: true, activeSessions: sessionRunning ? 1 : 0, appRunning: sessionRunning };
    } else if (path === '/api/apps/close' && request.method() === 'POST') {
      if (options.delayTerminateMs) {
        await new Promise((resolve) => setTimeout(resolve, options.delayTerminateMs));
      }
      sessionRunning = false;
      body = { status: true };
    } else if (path === '/api/webrtc/capabilities') {
      body = {
        enabled: true,
        availability: { state: 'ready' },
        codecs: {
          h264: { supported: true, hdr: false },
          hevc: { supported: false, hdr: false },
          av1: { supported: false, hdr: false },
        },
        hdr_policy: 'automatic',
        hdr_policy_allows: false,
        limits: {
          min_dimension: 64,
          max_dimension: 4096,
          min_fps: 1,
          max_fps: 240,
          min_bitrate_kbps: 0,
          max_bitrate_kbps: 500_000,
        },
      };
    } else if (path === '/api/webrtc/sessions' && request.method() === 'POST') {
      sessions.push(request.postDataJSON() as SessionPayload);
      body = options.failConnection
        ? { status: false, error: 'Browser stream fixture rejected the session.' }
        : { status: true, session: { id: 'browser-stream-test' }, ice_servers: [] };
    } else if (path.endsWith('/offer') && request.method() === 'POST') {
      body = {
        status: true,
        answer_ready: true,
        type: 'answer',
        sdp: 'v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 H264/90000\r\n',
      };
    } else if (path.endsWith('/ice')) {
      body = { candidates: [], next_since: 0 };
    } else if (path.startsWith('/api/webrtc/sessions/')) {
      body = { status: true };
    }
    await route.fulfill({ json: body });
  });

  return sessions;
}

for (const width of [1440, 390]) {
  test(`browser stream pacing and performance overlay remain usable at ${width}px`, async ({
    page,
  }) => {
    const sessions = await installBrowserStreamFixtures(page);
    await page.setViewportSize({ width, height: width === 390 ? 844 : 1000 });
    await page.goto('/v2/stream');

    await expect(page.getByRole('heading', { name: 'Stream settings' })).toBeVisible();
    await expect(page.locator('#browser-stream-pacing-slack')).toHaveValue('2');
    await expect(page.locator('#browser-stream-max-frame-age')).toHaveValue('1');
    await page.getByRole('button', { name: 'Smooth' }).click();
    await page.locator('#browser-stream-pacing-slack').fill('4');
    await page.locator('#browser-stream-max-frame-age').fill('3');
    await expect(page.locator('#browser-stream-max-frame-age')).toHaveValue('3');
    await expect
      .poll(() =>
        page.evaluate(() => {
          const saved = JSON.parse(localStorage.getItem('sunshine.webrtc.session_config') ?? '{}');
          return {
            mode: saved.videoPacingMode,
            slack: saved.videoPacingSlackMs,
            frames: saved.videoMaxFrameAgeFrames,
          };
        }),
      )
      .toEqual({ mode: 'smoothness', slack: 4, frames: 3 });
    await page.reload();
    await expect(page.locator('#browser-stream-pacing-slack')).toHaveValue('4');
    await expect(page.locator('#browser-stream-max-frame-age')).toHaveValue('3');
    const autoFullscreen = page.getByRole('checkbox', { name: 'Auto Fullscreen' });
    await expect(autoFullscreen).toBeChecked();
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-expanded.png`,
      fullPage: true,
    });
    const streamToggle = page.locator('[aria-controls="browser-stream-surface"]');
    await streamToggle.click();
    await expect(streamToggle).toHaveAttribute('aria-expanded', 'false');
    await expect(page.locator('#browser-stream-surface')).toBeHidden();
    await expect(page.locator('#browser-stream-surface video')).toHaveCount(1);
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-collapsed.png`,
      fullPage: true,
    });
    await streamToggle.click();
    await expect(streamToggle).toHaveAttribute('aria-expanded', 'true');
    await expect(page.locator('#browser-stream-surface')).toBeVisible();
    await autoFullscreen.uncheck();
    await expect
      .poll(() =>
        page.evaluate(
          () =>
            JSON.parse(localStorage.getItem('sunshine.webrtc.session_config') ?? '{}')
              .autoFullscreen,
        ),
      )
      .toBe(false);
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-preferences.png`,
      fullPage: true,
    });
    await autoFullscreen.check();
    await page.locator('#browser-stream-fps').fill('20');
    await expect(page.locator('#browser-stream-max-frame-age')).toHaveValue('2');
    await page.locator('#browser-stream-fps').fill('60');
    await page.locator('#browser-stream-max-frame-age').fill('3');
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-pacing.png`,
      fullPage: true,
    });

    await page.getByRole('button', { name: 'Start browser stream' }).click();
    await expect(page.getByText('Connected', { exact: true })).toBeVisible();
    await expect.poll(() => sessions.length).toBe(1);
    await streamToggle.scrollIntoViewIfNeeded();
    await streamToggle.click();
    await expect(streamToggle).toHaveAttribute('aria-expanded', 'false');
    await expect(
      page.getByText('Input forwarding pauses while the stream is minimized'),
    ).toBeVisible();
    await expect(page.getByRole('button', { name: 'Disconnect stream' })).toBeVisible();
    await streamToggle.focus();
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-active-collapsed.png`,
      fullPage: false,
    });
    await streamToggle.click();
    await expect(streamToggle).toHaveAttribute('aria-expanded', 'true');
    await expect
      .poll(() =>
        page.evaluate(
          () =>
            (window as typeof window & { __browserStreamFullscreenCalls?: string[] })
              .__browserStreamFullscreenCalls?.length ?? 0,
        ),
      )
      .toBeGreaterThan(0);
    expect(sessions[0]).toMatchObject({
      video_pacing_mode: 'smoothness',
      video_pacing_slack_ms: 4,
      video_max_frame_age_ms: 50,
    });

    await page.getByRole('checkbox', { name: 'Show performance overlay' }).check();
    await expect(page.locator('.stream-performance-overlay')).toContainText('FPS:');
    await expect(page.locator('.stream-performance-overlay')).toContainText('Bitrate:');
    await expect(page.locator('.stream-performance-overlay')).toContainText('Latency:');
    await expect(page.locator('.stream-performance-overlay')).toContainText('Dropped:');
    await expect(page.locator('.stream-performance-overlay')).toContainText('Latency: 24 ms RTT');
    await expect(page.locator('.stream-performance-overlay')).toContainText('Dropped: 2');
    await expect(page.locator('.stream-performance-overlay')).toContainText(/FPS:\s+\d+/);
    await expect(page.locator('.stream-performance-overlay')).toContainText(/Bitrate:\s+\S+/);
    await expect(page.locator('.stream-performance-overlay')).toContainText('video/H264');
    await expect(page.locator('.stream-performance-overlay')).toHaveCSS('pointer-events', 'none');
    const pointerInputCount = await page.evaluate(
      () =>
        (
          (window as typeof window & { __browserStreamInputs?: unknown[] }).__browserStreamInputs ??
          []
        ).filter(
          (message) =>
            Boolean(message) &&
            typeof message === 'object' &&
            ['mouse_down', 'mouse_up'].includes((message as { type?: string }).type ?? ''),
        ).length,
    );
    await page.locator('.stream-surface').click({ position: { x: 40, y: 40 } });
    await expect
      .poll(() =>
        page.evaluate((before) => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return (
            (messages?.filter(
              (message) =>
                Boolean(message) &&
                typeof message === 'object' &&
                ['mouse_down', 'mouse_up'].includes((message as { type?: string }).type ?? ''),
            ).length ?? 0) - before
          );
        }, pointerInputCount),
      )
      .toBe(2);
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages
            ?.slice()
            .reverse()
            .find(
              (message) =>
                Boolean(message) &&
                typeof message === 'object' &&
                (message as { type?: string }).type === 'gamepad_state',
            );
        }),
      )
      .toMatchObject({
        type: 'gamepad_state',
        gamepadType: 1,
        buttons: 0x1000,
        lsX: 14959,
        lsY: 0,
      });
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          const connect = messages?.find(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string }).type === 'gamepad_connect',
          ) as { capabilities?: number; supportedButtons?: number } | undefined;
          return connect;
        }),
      )
      .toMatchObject({ capabilities: 1 });
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          const connect = messages?.find(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string }).type === 'gamepad_connect',
          ) as { supportedButtons?: number } | undefined;
          return connect?.supportedButtons;
        }),
      )
      .toBeGreaterThan(0x1000);

    await page.getByRole('checkbox', { name: 'Forward browser input' }).uncheck();
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.some(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; buttons?: number }).type === 'gamepad_state' &&
              (message as { buttons?: number }).buttons === 0,
          );
        }),
      )
      .toBe(true);
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.some(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; id?: number }).type === 'gamepad_disconnect' &&
              (message as { id?: number }).id === 0,
          );
        }),
      )
      .toBe(true);
    await page.getByRole('checkbox', { name: 'Forward browser input' }).check();
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.filter(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; id?: number }).type === 'gamepad_connect' &&
              (message as { id?: number }).id === 0,
          ).length;
        }),
      )
      .toBeGreaterThan(1);

    await page.evaluate(() => {
      const gamepad = (
        window as typeof window & { __browserStreamGamepad?: { connected: boolean } }
      ).__browserStreamGamepad;
      if (gamepad) gamepad.connected = false;
    });
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.filter(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; id?: number }).type === 'gamepad_disconnect' &&
              (message as { id?: number }).id === 0,
          ).length;
        }),
      )
      .toBeGreaterThan(1);
    await page.evaluate(() => {
      const gamepad = (
        window as typeof window & { __browserStreamGamepad?: { connected: boolean } }
      ).__browserStreamGamepad;
      if (gamepad) gamepad.connected = true;
    });
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.filter(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; id?: number }).type === 'gamepad_connect' &&
              (message as { id?: number }).id === 0,
          ).length;
        }),
      )
      .toBeGreaterThan(2);

    await page.evaluate(() => {
      const channel = (
        window as typeof window & {
          __browserStreamInputChannel?: { onmessage?: (event: { data: string }) => void };
        }
      ).__browserStreamInputChannel;
      channel?.onmessage?.({
        data: JSON.stringify({
          type: 'gamepad_feedback',
          event: 'rumble',
          id: 0,
          lowfreq: 32767,
          highfreq: 16384,
        }),
      });
      window.dispatchEvent(new Event('blur'));
    });
    await expect
      .poll(() =>
        page.evaluate(() => {
          const messages = (window as typeof window & { __browserStreamInputs?: unknown[] })
            .__browserStreamInputs;
          return messages?.some(
            (message) =>
              Boolean(message) &&
              typeof message === 'object' &&
              (message as { type?: string; buttons?: number; lsX?: number }).type ===
                'gamepad_state' &&
              (message as { buttons?: number }).buttons === 0 &&
              (message as { lsX?: number }).lsX === 0,
          );
        }),
      )
      .toBe(true);
    await expect
      .poll(() =>
        page.evaluate(
          () =>
            (window as typeof window & { __browserStreamHaptics?: unknown[] })
              .__browserStreamHaptics?.length ?? 0,
        ),
      )
      .toBeGreaterThan(0);
    await page.screenshot({
      path: `/tmp/vibeshine-ui-results/browser-stream-${width}-overlay.png`,
      fullPage: true,
    });
  });
}

test('browser stream auto fullscreen respects opt-out and retains collapsed state on reconnect', async ({
  page,
}) => {
  await installBrowserStreamFixtures(page);
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/stream');

  const autoFullscreen = page.getByRole('checkbox', { name: 'Auto Fullscreen' });
  await autoFullscreen.uncheck();
  await page.getByRole('button', { name: 'Start browser stream' }).click();
  await expect(page.getByText('Connected', { exact: true })).toBeVisible();
  await expect
    .poll(() =>
      page.evaluate(
        () =>
          (window as typeof window & { __browserStreamFullscreenCalls?: string[] })
            .__browserStreamFullscreenCalls?.length ?? 0,
      ),
    )
    .toBe(0);

  const streamToggle = page.locator('[aria-controls="browser-stream-surface"]');
  await streamToggle.click();
  await expect(page.locator('#browser-stream-surface')).toBeHidden();
  await expect(page.locator('#browser-stream-surface video')).toHaveCount(1);
  await expect(page.getByRole('button', { name: 'Disconnect stream' })).toBeVisible();
  await expect(page.getByRole('checkbox', { name: 'Forward browser input' })).toBeChecked();

  await page.getByRole('button', { name: 'Disconnect stream' }).click();
  await expect(page.getByRole('button', { name: 'Start browser stream' })).toBeVisible();
  await expect(page.locator('#browser-stream-surface')).toBeHidden();

  await page.getByRole('button', { name: 'Start browser stream' }).click();
  await expect(page.getByText('Connected', { exact: true })).toBeVisible();
  await expect(page.locator('#browser-stream-surface')).toBeHidden();
  await expect(streamToggle).toHaveAttribute('aria-expanded', 'false');
});

test('browser stream falls back when the browser rejects native fullscreen', async ({ page }) => {
  await installBrowserStreamFixtures(page, { rejectFullscreen: true });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/stream');
  await page.getByRole('button', { name: 'Start browser stream' }).click();
  await expect(page.getByText('Connected', { exact: true })).toBeVisible();
  await expect(page.locator('.stream-surface--pseudo-fullscreen')).toBeVisible();

  await page.getByRole('button', { name: 'Exit fullscreen' }).click();
  await expect(page.locator('.stream-surface--pseudo-fullscreen')).toHaveCount(0);
  await page.getByRole('button', { name: 'Disconnect stream' }).click();
  await expect(page.locator('.stream-surface--pseudo-fullscreen')).toHaveCount(0);
});

test('browser stream requests fullscreen from the terminate confirmation gesture', async ({
  page,
}) => {
  await installBrowserStreamFixtures(page, {
    delayTerminateMs: 300,
    includeApp: true,
    runningSession: true,
  });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/stream');

  const streamToggle = page.locator('[aria-controls="browser-stream-surface"]');
  await streamToggle.click();
  await expect(page.getByRole('button', { name: 'Terminate' })).toBeVisible();
  await streamToggle.click();
  await page.getByRole('option', { name: 'Test Game' }).click();
  await page.getByRole('button', { name: 'Start browser stream' }).click();
  await expect(page.getByRole('dialog')).toBeVisible();
  await expect
    .poll(() =>
      page.evaluate(
        () =>
          (window as typeof window & { __browserStreamFullscreenCalls?: string[] })
            .__browserStreamFullscreenCalls?.length ?? 0,
      ),
    )
    .toBe(0);

  await page.getByRole('button', { name: 'Terminate & Start' }).click();
  // The close request is intentionally delayed. A fullscreen call observed
  // while it is pending proves it began in the trusted confirmation event.
  await expect
    .poll(() =>
      page.evaluate(
        () =>
          (window as typeof window & { __browserStreamFullscreenCalls?: string[] })
            .__browserStreamFullscreenCalls?.length ?? 0,
      ),
    )
    .toBeGreaterThan(0);
  await expect(page.getByText('Connected', { exact: true })).toBeVisible();
});

test('browser stream exits auto fullscreen when connection startup fails', async ({ page }) => {
  await installBrowserStreamFixtures(page, { failConnection: true, rejectFullscreen: true });
  await page.setViewportSize({ width: 1440, height: 1000 });
  await page.goto('/v2/stream');
  await page.getByRole('button', { name: 'Start browser stream' }).click();

  await expect(page.getByText(/WebRTC session identifier/)).toBeVisible();
  await expect(page.locator('.stream-surface--pseudo-fullscreen')).toHaveCount(0);
  await expect
    .poll(() =>
      page.evaluate(
        () =>
          (window as typeof window & { __browserStreamFullscreenCalls?: string[] })
            .__browserStreamFullscreenCalls?.length ?? 0,
      ),
    )
    .toBeGreaterThan(0);
});
