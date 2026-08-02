import fs from 'fs';
import { resolve } from 'path';
import { defineConfig } from 'vite';
import type { Plugin } from 'vite';
import Components from 'unplugin-vue-components/vite';
import { NaiveUiResolver } from 'unplugin-vue-components/resolvers';
import vue from '@vitejs/plugin-vue';
import { ViteEjsPlugin } from 'vite-plugin-ejs';
import { fileURLToPath } from 'url';
import { parseBundledReleaseNote, sortChangelogEntries } from './utils/changelog';

// Resolve directory of this config file (works even if the folder was moved)
const CONFIG_DIR = fileURLToPath(new URL('.', import.meta.url));

// Find the repo root by walking up until a CMakeLists.txt is found (best-effort, capped depth)
function findRepoRoot(startDir: string): string {
  let dir = startDir;
  for (let i = 0; i < 8; i++) {
    if (fs.existsSync(resolve(dir, 'CMakeLists.txt'))) return dir;
    const parent = resolve(dir, '..');
    if (parent === dir) break;
    dir = parent;
  }
  return startDir;
}

// Legacy is the default interface and builds at the web root.
function resolveAssetsDstPath(): string {
  const repoRoot = findRepoRoot(CONFIG_DIR);
  const dst = process.env.SUNSHINE_LEGACY_WEB_OUTPUT_DIR
    ? resolve(process.env.SUNSHINE_LEGACY_WEB_OUTPUT_DIR)
    : resolve(repoRoot, 'build/assets/web');
  const v2Output = resolve(repoRoot, 'build/assets/web/v2');

  if (dst.toLowerCase() === v2Output.toLowerCase()) {
    throw new Error('SUNSHINE_LEGACY_WEB_OUTPUT_DIR must not overwrite the v2 web output.');
  }

  return dst;
}

const assetsSrcPath = CONFIG_DIR;
const assetsDstPath = resolveAssetsDstPath();

const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'), 'utf-8');

function getManualChunk(id: string): string | undefined {
  const normalized = id.replace(/\\/g, '/');
  const nodeModulesMarker = '/node_modules/';
  const markerIndex = normalized.indexOf(nodeModulesMarker);
  if (markerIndex === -1) {
    return undefined;
  }

  const packagePath = normalized.slice(markerIndex + nodeModulesMarker.length);
  const packageName = packagePath.startsWith('@')
    ? packagePath.split('/').slice(0, 2).join('/')
    : (packagePath.split('/')[0] ?? '');

  if (
    packageName === 'vue' ||
    packageName === 'pinia' ||
    packageName === 'vue-router' ||
    packageName === 'vue-i18n' ||
    packageName.startsWith('@vue/') ||
    packageName.startsWith('@intlify/')
  ) {
    return 'vue-core';
  }

  if (
    packageName === 'naive-ui' ||
    packageName === 'vueuc' ||
    packageName === 'vooks' ||
    packageName === 'css-render' ||
    packageName === 'seemly' ||
    packageName === 'evtd'
  ) {
    // Keep the Naive UI stack in the general vendor chunk. Splitting it into its
    // own manual chunk can create a circular import with the remaining vendor
    // bundle in release builds, which prevents the app from mounting at all.
    return 'vendor';
  }

  if (packageName.startsWith('@fortawesome/')) {
    return 'fontawesome';
  }

  return 'vendor';
}

function bundledChangelogPlugin(repoRoot: string): Plugin {
  return {
    name: 'vibepollo-bundled-changelog',
    generateBundle() {
      const releaseNotesDir = resolve(repoRoot, 'release_notes');
      const releases = fs.existsSync(releaseNotesDir)
        ? fs
            .readdirSync(releaseNotesDir)
            .filter((name) => name.toLowerCase().endsWith('.md'))
            .map((name) => {
              const path = resolve(releaseNotesDir, name);
              return parseBundledReleaseNote(name, fs.readFileSync(path, 'utf-8'));
            })
            .filter((entry) => entry !== null)
        : [];

      this.emitFile({
        type: 'asset',
        fileName: 'assets/changelog.json',
        source: JSON.stringify(
          {
            generatedAt: new Date().toISOString(),
            releases: sortChangelogEntries(releases),
          },
          null,
          2,
        ),
      });
    },
  };
}

export default defineConfig(({ mode }) => {
  const isDebug = mode === 'debug';
  const repoRoot = findRepoRoot(CONFIG_DIR);

  return {
    root: resolve(assetsSrcPath),
    base: '/',
    resolve: {
      alias: { '@': resolve(assetsSrcPath) },
    },
    // Compile-time Vue feature flags
    // - Keep Options API off
    // - Enable Vue devtools in production when building Debug bundles
    define: {
      __VUE_OPTIONS_API__: false,
      __VUE_PROD_DEVTOOLS__: isDebug,
    },
    plugins: [
      vue(),
      Components({
        // Auto-import only used Naive UI components to minimize bundle
        resolvers: [NaiveUiResolver()],
        dts: false,
      }),
      bundledChangelogPlugin(repoRoot),
      ViteEjsPlugin({ header }),
    ],
    css: {
      // Include CSS sources in sourcemaps during debug
      devSourcemap: isDebug,
    },
    build: {
      outDir: resolve(assetsDstPath),
      sourcemap: isDebug ? 'inline' : false,
      emptyOutDir: true,
      chunkSizeWarningLimit: 600,
      minify: isDebug ? false : 'esbuild',
      rollupOptions: {
        input: { index: resolve(assetsSrcPath, 'index.html') },
        output: {
          manualChunks: getManualChunk,
        },
      },
    },
  };
});
