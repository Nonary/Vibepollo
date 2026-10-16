import { test as base, expect } from '@playwright/test';
import type { Page } from '@playwright/test';

export { expect } from '@playwright/test';
export type { Locator, Page } from '@playwright/test';

export type UpdaterNetworkAudit = {
  defaultReleaseRequests: string[];
  defaultMetadataRequests: string[];
  observedExternalRequests: string[];
  unmatchedExternalRequests: string[];
};

function testOrigins(baseURL: string | undefined): Set<string> {
  const origins = new Set<string>();
  for (const value of [baseURL, process.env.VIBEPOLLO_LEGACY_TEST_URL]) {
    if (value) origins.add(new URL(value).origin);
  }
  return origins;
}

export async function localRoute(
  page: Page,
  pattern: string,
  handler: Parameters<Page['route']>[1],
): Promise<void> {
  const origins = testOrigins(test.info().project.use.baseURL);
  await page.route(pattern, async (route, request) => {
    const url = new URL(request.url());
    if (!origins.has(url.origin) ||
        (pattern === '**/api/**' && url.pathname === '/api/updates/nightly-metadata')) {
      await route.fallback();
      return;
    }
    await handler(route, request);
  });
}

export const test = base.extend<{ updaterNetworkAudit: UpdaterNetworkAudit }>({
  updaterNetworkAudit: [async ({ context }, use, testInfo) => {
    const audit: UpdaterNetworkAudit = {
      defaultReleaseRequests: [],
      defaultMetadataRequests: [],
      observedExternalRequests: [],
      unmatchedExternalRequests: [],
    };
    const localOrigins = testOrigins(testInfo.project.use.baseURL);
    const releaseRequest = (url: URL) => url.protocol === 'https:' &&
      url.hostname === 'api.github.com' &&
      url.pathname.toLowerCase() === '/repos/nonary/vibepollo/releases';
    context.on('request', (request) => {
      const url = new URL(request.url());
      if (/^https?:$/.test(url.protocol) && !localOrigins.has(url.origin))
        audit.observedExternalRequests.push(request.url());
    });
    // Explicit page routes take precedence, so updater/provider fixtures retain their responses.
    // This context fallback makes default-enabled release checks deterministic without opting out.
    await context.route('**/*', async (route) => {
      const url = new URL(route.request().url());
      if (releaseRequest(url) && route.request().method() === 'GET') {
        audit.defaultReleaseRequests.push(route.request().url());
        await route.fulfill({ json: [] });
      } else if (localOrigins.has(url.origin) && url.pathname === '/api/updates/nightly-metadata') {
        audit.defaultMetadataRequests.push(route.request().url());
        await route.fulfill({ status: 404, json: { message: 'No nightly metadata fixture installed' } });
      } else if (localOrigins.has(url.origin) || !/^https?:$/.test(url.protocol)) {
        await route.fallback();
      } else {
        // An unmatched external request is a missing fixture, not permission to use a live service.
        audit.unmatchedExternalRequests.push(route.request().url());
        await route.abort('blockedbyclient');
      }
    });
    try {
      await use(audit);
    } finally {
      await testInfo.attach('updater-network-audit', {
        body: Buffer.from(JSON.stringify(audit, null, 2)),
        contentType: 'application/json',
      });
      expect(audit.unmatchedExternalRequests, 'Install an explicit page fixture for every external request').toEqual([]);
    }
  }, { auto: true }],
});
