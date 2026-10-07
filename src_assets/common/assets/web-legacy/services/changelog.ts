import { useConfigStore } from '@/stores/config';
import {
  type BundledChangelogAsset,
  type ChangelogEntry,
  type GitHubReleaseLike,
  compareChangelogTags,
  githubReleaseToChangelogEntry,
  mergeChangelogEntries,
  sortChangelogEntries,
} from '@/utils/changelog';

export interface LoadChangelogResult {
  releases: ChangelogEntry[];
  bundledOnly: boolean;
  githubError: string | null;
  latestAvailable: ChangelogEntry | null;
  installedVersion: string;
}

const CHANGELOG_ASSET_URL = './assets/changelog.json';
const GITHUB_RELEASES_URL = 'https://api.github.com/repos/Nonary/Vibepollo/releases';

function isChangelogEntry(value: unknown): value is ChangelogEntry {
  return !!value && typeof value === 'object' && typeof (value as ChangelogEntry).tag === 'string';
}

export async function getInstalledVersion(): Promise<string> {
  try {
    const configStore = useConfigStore();
    const cfg = await configStore.fetchConfig();
    const version = configStore.metadata?.version || cfg?.['version'];
    return typeof version === 'string' && version.trim() ? version.trim() : '0.0.0';
  } catch {
    return '0.0.0';
  }
}

export async function loadBundledChangelog(): Promise<ChangelogEntry[]> {
  const response = await fetch(CHANGELOG_ASSET_URL, { cache: 'no-cache' });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const data = (await response.json()) as Partial<BundledChangelogAsset>;
  const releases = Array.isArray(data.releases) ? data.releases.filter(isChangelogEntry) : [];
  return sortChangelogEntries(releases);
}

export async function loadGithubChangelog(): Promise<ChangelogEntry[]> {
  const response = await fetch(GITHUB_RELEASES_URL, {
    headers: { Accept: 'application/vnd.github+json' },
  });
  if (!response.ok) throw new Error(`HTTP ${response.status}`);
  const releases = (await response.json()) as GitHubReleaseLike[];
  if (!Array.isArray(releases)) return [];
  return releases
    .map(githubReleaseToChangelogEntry)
    .filter((entry): entry is ChangelogEntry => entry !== null);
}

export async function loadChangelog(): Promise<LoadChangelogResult> {
  const [installedVersion, bundled] = await Promise.all([
    getInstalledVersion(),
    loadBundledChangelog().catch(() => []),
  ]);

  let github: ChangelogEntry[] = [];
  let githubError: string | null = null;
  try {
    github = await loadGithubChangelog();
  } catch (error) {
    githubError = error instanceof Error ? error.message : String(error);
  }

  const releases = mergeChangelogEntries(bundled, github);
  const latestAvailable =
    releases.reduce<ChangelogEntry | null>((latest, release) => {
      if (!latest) return release;
      return compareChangelogTags(release.tag, latest.tag) > 0 ? release : latest;
    }, null) ?? null;
  return {
    releases,
    bundledOnly: github.length === 0,
    githubError,
    latestAvailable,
    installedVersion,
  };
}
