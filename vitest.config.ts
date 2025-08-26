import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'node:path';

export default defineConfig({
  plugins: [vue()],
  resolve: {
    alias: {
      '@web': resolve(__dirname, 'src_assets/common/assets/web'),
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: [resolve(__dirname, 'tests/frontend/setup.ts')],
    include: ['tests/frontend/**/*.test.ts'],
    css: true,
  },
});
