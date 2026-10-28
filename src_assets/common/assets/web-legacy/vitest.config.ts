import { defineConfig } from 'vitest/config';
import vue from '@vitejs/plugin-vue';
import { resolve } from 'node:path';

const repoRoot = resolve(__dirname, '../../../..');
const normalizePath = (path: string) => path.replace(/\\/g, '/');

export default defineConfig({
  plugins: [vue() as unknown as never],
  resolve: {
    alias: {
      '@web': normalizePath(__dirname),
      '@': normalizePath(__dirname),
      pinia: resolve(__dirname, 'node_modules/pinia/dist/pinia.mjs'),
      'vue-i18n': resolve(__dirname, 'node_modules/vue-i18n/dist/vue-i18n.mjs'),
      'naive-ui': resolve(__dirname, 'node_modules/naive-ui/es/index.mjs'),
      vue: resolve(__dirname, 'node_modules/vue/dist/vue.runtime.esm-bundler.js'),
      '@vue/test-utils': resolve(
        __dirname,
        'node_modules/@vue/test-utils/dist/vue-test-utils.esm-bundler.mjs',
      ),
    },
  },
  server: {
    fs: {
      allow: [repoRoot, __dirname],
    },
  },
  test: {
    environment: 'jsdom',
    globals: true,
    setupFiles: [resolve(repoRoot, 'tests/frontend/setup.ts')],
    include: [normalizePath(resolve(repoRoot, 'tests/frontend/**/*.test.ts'))],
    css: true,
  },
});
