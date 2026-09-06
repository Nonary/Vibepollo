export type GameProvider = 'playnite' | 'steam' | 'lutris';

export interface LibraryGame {
  provider: GameProvider;
  id: string;
  name: string;
}

export const providerLabels: Record<GameProvider, string> = {
  playnite: 'Playnite',
  steam: 'Steam',
  lutris: 'Lutris',
};

const priority: Record<GameProvider, number> = { playnite: 0, steam: 1, lutris: 2 };

function titleKey(name: string): string {
  return name.normalize('NFKC').trim().replace(/\s+/g, ' ').toLowerCase();
}

// Keep editions and sequels distinct; only normalize casing and whitespace.
export function groupLibraryGames<T extends LibraryGame>(games: T[], query = '') {
  const groups = new Map<string, T[]>();
  const search = titleKey(query);
  for (const game of games) {
    const key = titleKey(game.name);
    if (!key || !key.includes(search)) continue;
    const entries = groups.get(key) ?? [];
    if (!entries.some((entry) => entry.provider === game.provider && entry.id === game.id)) {
      entries.push(game);
    }
    groups.set(key, entries);
  }
  return [...groups.entries()]
    .map(([key, entries]) => {
      entries.sort((left, right) => priority[left.provider] - priority[right.provider]);
      return { key, name: entries[0]!.name, entries };
    })
    .sort((left, right) => left.name.localeCompare(right.name));
}
