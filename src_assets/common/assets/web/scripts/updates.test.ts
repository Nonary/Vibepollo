import assert from 'node:assert/strict';
import { test } from 'node:test';
import { selectAvailableUpdate } from '../utils/updates.ts';

const releases = [
  { tag_name: 'v2.0.0-beta.10', prerelease: true },
  { tag_name: 'v1.9.0-stable.2', prerelease: false },
  { tag_name: 'v2.0.0-beta.2', prerelease: true },
  { tag_name: 'v3.0.0', draft: true },
];

test('stable installs only see newer stable releases by default', () => {
  assert.equal(selectAvailableUpdate('1.9.0-stable.1', releases, false)?.tag, '1.9.0-stable.2');
  assert.equal(selectAvailableUpdate('1.9.0-stable.2', releases, false), null);
  assert.equal(selectAvailableUpdate('3.0.0', releases, true), null);
});

test('prerelease installs follow their channel and compare numeric suffixes', () => {
  assert.equal(selectAvailableUpdate('2.0.0-beta.2', releases, false)?.tag, '2.0.0-beta.10');
  assert.equal(selectAvailableUpdate('2.0.0-beta.10', releases, false), null);
});

test('explicit prerelease opt-in supports both config representations', () => {
  for (const enabled of [true, 'enabled']) {
    assert.equal(selectAvailableUpdate('1.9.0-stable.2', releases, enabled)?.tag, '2.0.0-beta.10');
  }
});

test('stable release supersedes a beta and unknown installed versions produce no notice', () => {
  assert.equal(
    selectAvailableUpdate('2.0.0-beta.10', [...releases, { tag_name: 'v2.0.0' }], false)?.tag,
    '2.0.0',
  );
  assert.equal(selectAvailableUpdate('', releases, true), null);
  assert.equal(selectAvailableUpdate('unknown', releases, true), null);
});
