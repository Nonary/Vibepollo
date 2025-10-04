import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'node:path';

const repoRoot = resolve(__dirname, '../../../..');

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@web': __dirname,
      '@': __dirname,
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: [resolve(repoRoot, 'tests/frontend/setup.ts')],
    include: [resolve(repoRoot, 'tests/frontend/**/*.test.ts')],
    css: true,
  },
});
