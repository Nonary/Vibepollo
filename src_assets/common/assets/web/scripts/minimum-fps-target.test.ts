import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { getConfigFieldDefinition } from '../configs/configFieldSchema.ts';
import { settingsDefaults } from '../configs/settingsSchema.ts';

const root = new URL('../../../../../', import.meta.url);
const read = (path: string) => readFileSync(new URL(path, root), 'utf8');
const backendDefault = Number(
  read('src/config.cpp').match(/(\d+),\s*\/\/ minimum_fps_target/)?.[1],
);

test('minimum FPS placeholder and settings default match the backend default', () => {
  assert.equal(backendDefault, 20);
  assert.equal(settingsDefaults.minimum_fps_target, backendDefault);
  const field = getConfigFieldDefinition('minimum_fps_target', {
    t: (key: string) => key,
    platform: 'windows',
  });
  assert.equal(field.placeholder, String(backendDefault));
  const legacy = read('src_assets/common/assets/web-legacy/configs/configFieldSchema.ts');
  assert.match(legacy, new RegExp(`minimum_fps_target: \\{[^}]*placeholder: '${backendDefault}'`));
});

test('minimum FPS documented default matches the backend default', () => {
  const section = read('docs/configuration.md')
    .split('### minimum_fps_target')[1]
    .split('## Network')[0];
  const documentedDefault = section.match(
    /<td>Default<\/td>\s*<td colspan="2">@code\{\}\s*(\d+)/,
  )?.[1];
  assert.equal(Number(documentedDefault), backendDefault);
});
