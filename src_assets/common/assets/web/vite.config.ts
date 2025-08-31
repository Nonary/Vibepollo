import fs from 'fs';
import { resolve } from 'path';
import { defineConfig } from 'vite';
import Components from 'unplugin-vue-components/vite';
import { NaiveUiResolver } from 'unplugin-vue-components/resolvers';
import vue from '@vitejs/plugin-vue';
import { ViteEjsPlugin } from 'vite-plugin-ejs';
import { fileURLToPath } from 'url';

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

// Resolve source assets directory, supporting legacy env override and new layouts
function resolveAssetsSrcPath(): string {
  let src = CONFIG_DIR; // default to the folder containing this config

  if (!process.env['SUNSHINE_BUILD_HOMEBREW'] && process.env['SUNSHINE_SOURCE_ASSETS_DIR']) {
    const override = fs.realpathSync(process.env['SUNSHINE_SOURCE_ASSETS_DIR'] as string);
    // If override points directly to a folder with index.html, use it
    if (fs.existsSync(resolve(override, 'index.html'))) {
      src = override;
    } else if (fs.existsSync(resolve(override, 'common/assets/web/index.html'))) {
      // Backward-compat with original layout where override was repo/src_assets root
      src = resolve(override, 'common/assets/web');
    } else if (fs.existsSync(resolve(override, 'assets/web/index.html'))) {
      // Alternate layout where override is repo root
      src = resolve(override, 'assets/web');
    } else {
      // Fallback to override itself if it exists
      src = override;
    }
  }

  return src;
}

// Resolve destination assets directory; defaults to <repoRoot>/build/assets/web
function resolveAssetsDstPath(): string {
  const repoRoot = findRepoRoot(CONFIG_DIR);
  let dst = resolve(repoRoot, 'build/assets/web');

  if (!process.env['SUNSHINE_BUILD_HOMEBREW'] && process.env['SUNSHINE_ASSETS_DIR']) {
    // Keep legacy behavior: env points to install root, append assets/web
    dst = resolve(fs.realpathSync(process.env['SUNSHINE_ASSETS_DIR'] as string), 'assets/web');
  }

  return dst;
}

const assetsSrcPath = resolveAssetsSrcPath();
const assetsDstPath = resolveAssetsDstPath();

const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'), 'utf-8');

export default defineConfig(({ mode }) => {
  const isDebug = mode === 'debug';

  return {
    root: resolve(assetsSrcPath),
    base: './',
    resolve: {
      alias: { '@': resolve(assetsSrcPath) },
    },
    // Help Rollup/ESBuild drop unused Vue features for smaller bundles
    define: {
      __VUE_OPTIONS_API__: false,
      __VUE_PROD_DEVTOOLS__: false,
    },
    plugins: [
      vue(),
      Components({
        // Auto-import only used Naive UI components to minimize bundle
        resolvers: [NaiveUiResolver()],
        dts: false,
      }),
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
      minify: isDebug ? false : 'esbuild',
      rollupOptions: {
        input: { index: resolve(assetsSrcPath, 'index.html') },
      },
    },
  };
});
