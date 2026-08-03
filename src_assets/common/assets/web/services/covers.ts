import { apiPost } from '@/api/client';
import type { CoverCandidate } from '@/components/app-edit/AppEditCoverModal.types';

const GAME_DATABASE_URL = 'https://raw.githubusercontent.com/LizardByte/GameDB/gh-pages';

interface GameDatabaseBucketEntry {
  name?: unknown;
}

interface GameDatabaseRecord {
  id?: unknown;
  name?: unknown;
  cover?: {
    url?: unknown;
  };
}

interface CoverUploadResponse extends Record<string, unknown> {
  path?: unknown;
}

interface PlayniteCoverResponse extends Record<string, unknown> {
  status?: unknown;
}

function searchBucket(name: string): string {
  const prefix = name
    .substring(0, Math.min(name.length, 2))
    .toLocaleLowerCase()
    .replace(/[^a-z\d]/g, '');
  return prefix || '@';
}

function coverCandidate(game: GameDatabaseRecord): CoverCandidate | null {
  const id = typeof game.id === 'number' || typeof game.id === 'string' ? String(game.id) : '';
  const name = typeof game.name === 'string' ? game.name : '';
  const thumbnail = typeof game.cover?.url === 'string' ? game.cover.url : '';
  const dotIndex = thumbnail.lastIndexOf('.');
  const slashIndex = thumbnail.lastIndexOf('/');
  if (!id || !name || dotIndex < 0 || slashIndex < 0) return null;

  const slug = thumbnail.substring(slashIndex + 1, dotIndex);
  if (!slug) return null;
  return {
    name,
    key: `igdb_${id}`,
    url: `https://images.igdb.com/igdb/image/upload/t_cover_big/${slug}.jpg`,
    saveUrl: `https://images.igdb.com/igdb/image/upload/t_cover_big_2x/${slug}.png`,
  };
}

export async function searchCovers(name: string): Promise<CoverCandidate[]> {
  const trimmedName = name.trim();
  if (!trimmedName) return [];

  const response = await fetch(`${GAME_DATABASE_URL}/buckets/${searchBucket(trimmedName)}.json`);
  if (!response.ok) throw new Error('cover-search-failed');

  const bucket = (await response.json()) as Record<string, GameDatabaseBucketEntry>;
  const normalizedSearch = trimmedName.replace(/\s+/g, '.').toLocaleLowerCase();
  const matchingIds = Object.entries(bucket ?? {})
    .filter(([, item]) =>
      typeof item?.name === 'string'
        ? item.name.replace(/\s+/g, '.').toLocaleLowerCase().startsWith(normalizedSearch)
        : false,
    )
    .map(([id]) => id);

  const games = await Promise.all(
    matchingIds.map(async (id): Promise<GameDatabaseRecord | null> => {
      try {
        const gameResponse = await fetch(
          `${GAME_DATABASE_URL}/games/${encodeURIComponent(id)}.json`,
        );
        return gameResponse.ok ? ((await gameResponse.json()) as GameDatabaseRecord) : null;
      } catch {
        return null;
      }
    }),
  );

  return games
    .map((game) => (game ? coverCandidate(game) : null))
    .filter((candidate): candidate is CoverCandidate => candidate !== null);
}

export async function uploadCover(cover: CoverCandidate): Promise<string> {
  const response = await apiPost<CoverUploadResponse>('/api/covers/upload', {
    key: cover.key,
    url: cover.saveUrl,
  });
  if (typeof response.path !== 'string' || !response.path) throw new Error('cover-upload-failed');
  return response.path;
}

export async function updatePlayniteCover(
  playniteId: string,
  cover: CoverCandidate,
): Promise<void> {
  const response = await apiPost<PlayniteCoverResponse>('/api/playnite/cover', {
    playnite_id: playniteId,
    cover_key: cover.key,
  });
  if (response.status !== true) throw new Error('playnite-cover-update-failed');
}
