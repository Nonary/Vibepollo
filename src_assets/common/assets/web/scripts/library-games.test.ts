import assert from 'node:assert/strict';
import test from 'node:test';
import { groupLibraryGames } from '../../web-legacy/utils/libraryGames.ts';

test('groups matching titles and defaults to Playnite, Steam, then Lutris', () => {
  const games = groupLibraryGames([
    { provider: 'lutris', id: '1', name: ' Portal  2 ' },
    { provider: 'steam', id: '620', name: 'Portal 2' },
    { provider: 'playnite', id: 'abc', name: 'PORTAL 2' },
    { provider: 'steam', id: '620', name: 'Portal 2' },
  ]);
  assert.equal(games.length, 1);
  assert.deepEqual(
    games[0]?.entries.map((entry) => entry.provider),
    ['playnite', 'steam', 'lutris'],
  );
});

test('keeps single-library games, editions, and sequels separate', () => {
  const games = groupLibraryGames(
    [
      { provider: 'steam', id: '1', name: 'Portal' },
      { provider: 'steam', id: '2', name: 'Portal 2' },
      { provider: 'lutris', id: '3', name: 'Portal 2 Special Edition' },
    ],
    'PORTAL',
  );
  assert.equal(games.length, 3);
  assert.ok(games.every((group) => group.entries.length === 1));
});
