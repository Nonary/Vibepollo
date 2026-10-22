import { defineConfig } from '@playwright/test';
import base from './playwright.config';

export default defineConfig({
  ...base,
  testMatch: 'application-display.spec.ts',
  testIgnore: [],
  webServer: [
    ...(Array.isArray(base.webServer) ? base.webServer : [base.webServer!]),
    {
      command:
        'npm run build && node node_modules/serve/build/main.js --single /tmp/vibeshine-app-display-legacy --listen tcp://127.0.0.1:5174',
      env: { SUNSHINE_LEGACY_WEB_OUTPUT_DIR: '/tmp/vibeshine-app-display-legacy' },
      cwd: '../web-legacy',
      url: 'http://127.0.0.1:5174/applications',
      reuseExistingServer: !process.env.CI,
    },
  ],
});
