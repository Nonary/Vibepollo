import { join } from 'node:path';
import { tmpdir } from 'node:os';
import { defineConfig } from '@playwright/test';
export default defineConfig({
  testDir: './e2e',
  outputDir: process.env.VIBEPOLLO_TEST_OUTPUT || process.env.VIBESHINE_TEST_OUTPUT || join(tmpdir(), 'vibepollo-ui-results'),
  fullyParallel: true,
  workers: 2,
  use: {
    baseURL: 'http://127.0.0.1:5173',
    viewport: { width: 1440, height: 1000 },
    trace: 'retain-on-failure',
    launchOptions: { executablePath: process.env.PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH },
  },
  webServer: {
    command: 'npm run dev -- --strictPort',
    url: 'http://127.0.0.1:5173/v2/',
    reuseExistingServer: !process.env.CI,
  },
});
