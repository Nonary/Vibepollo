import fs from 'fs';
import { resolve } from 'path';
import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { ViteEjsPlugin } from 'vite-plugin-ejs';

// When colocated under the web assets folder, default to current dir as source
let assetsSrcPath = '.';

// Default output goes to repo-root/build/assets/web unless overridden by env
const repoRoot = resolve(__dirname, '../../../..');
let assetsDstPath = resolve(repoRoot, 'build/assets/web');

// Allow CMake or packaging to override paths (and resolve symlinks)
if (!process.env.SUNSHINE_BUILD_HOMEBREW) {
  if (process.env.SUNSHINE_SOURCE_ASSETS_DIR) {
    assetsSrcPath = resolve(
      fs.realpathSync(process.env.SUNSHINE_SOURCE_ASSETS_DIR),
      'common/assets/web',
    );
  }
  if (process.env.SUNSHINE_ASSETS_DIR) {
    assetsDstPath = resolve(fs.realpathSync(process.env.SUNSHINE_ASSETS_DIR), 'assets/web');
  }
}

const header = fs.readFileSync(resolve(assetsSrcPath, 'template_header.html'), 'utf-8');

export default defineConfig(({ mode }) => {
  const isDebug = mode === 'debug';

  // Dynamically include only HTML entry points that exist. Primary SPA is index.html.
  const candidatePages = [
    'index',
    'apps',
    'playnite',
    'clients',
    'config',
    'password',
    'troubleshooting',
    'pin',
  ];
  const input: Record<string, string> = {};
  for (const name of candidatePages) {
    const p = resolve(assetsSrcPath, `${name}.html`);
    if (fs.existsSync(p)) input[name] = p;
  }

  return {
    root: resolve(assetsSrcPath),
    base: './',
    resolve: {
      alias: { '@': resolve(assetsSrcPath) },
    },
    plugins: [vue(), ViteEjsPlugin({ header })],
    css: {
      devSourcemap: isDebug,
    },
    build: {
      outDir: assetsDstPath,
      sourcemap: isDebug ? 'inline' : false,
      minify: isDebug ? false : 'esbuild',
      rollupOptions: { input },
    },
  };
});
