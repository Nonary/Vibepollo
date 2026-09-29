import assert from 'node:assert/strict';
import test from 'node:test';
import {
  appBooleanChoice,
  parseAppExtras,
  setAppOption,
  setAppStateCommand,
} from '../utils/appCompatibility.ts';

test('editing one app option preserves absent defaults and legacy or unknown values', () => {
  const original = {
    'allow-client-commands': false,
    'scale-factor': '125',
    'use-app-identity': 'false',
    gamepad: 'legacy-custom-controller',
    'future-option': { keep: [1, 2] },
  };
  const loaded = parseAppExtras(JSON.stringify(original))!;
  assert.deepEqual(loaded, original);
  const changed = setAppOption(loaded, 'terminate-on-pause', true);
  assert.deepEqual(changed, { ...original, 'terminate-on-pause': true });
  assert.equal('exclude-global-state-cmd' in changed, false);
  assert.deepEqual(setAppOption(changed, 'terminate-on-pause', undefined), original);
  for (const value of ['false', false, '0']) assert.equal(appBooleanChoice(value), 'false');
  assert.equal(appBooleanChoice(undefined), '');
  assert.equal(appBooleanChoice('future-value'), 'future-value');
});

test('state command edits preserve other commands and command metadata', () => {
  const original = {
    'state-cmd': [
      { do: 'old', undo: 'restore', elevated: 'false', extension: { preserve: true } },
      { do: 'second', undo: '', elevated: true },
    ],
    'allow-client-commands': false,
  };
  const changed = setAppStateCommand(original, 0, 'do', 'new');
  assert.deepEqual(changed, {
    ...original,
    'state-cmd': [{ ...original['state-cmd'][0], do: 'new' }, original['state-cmd'][1]],
  });
  assert.equal(original['state-cmd'][0].do, 'old');
  assert.equal(setAppStateCommand(original, 4, 'do', 'bad'), original);
});

test('invalid extras cannot silently become an empty app record', () => {
  for (const invalid of ['{', 'null', '[]', 'false']) assert.equal(parseAppExtras(invalid), null);
  assert.deepEqual(parseAppExtras(''), {});
});
