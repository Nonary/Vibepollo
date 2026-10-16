import {
  compareChangelogTags,
  parseChangelogVersion,
  githubReleaseToChangelogEntry,
  type ChangelogEntry,
  type GitHubReleaseLike,
} from './changelog.ts';

export interface UpdateRelease extends GitHubReleaseLike {
  assets?: { name?: string; state?: string; size?: number }[];
  nightly_metadata?: Record<string, unknown>;
}

export function prereleasesEnabled(value: unknown): boolean {
  // A missing key is the migration path. Every explicit disabled form wins,
  // including when the installed version itself is a prerelease.
  return (
    value === undefined ||
    value === null ||
    value === true ||
    value === 'enabled' ||
    value === 'true' ||
    value === 1 ||
    value === '1'
  );
}

function orderedVersion(value: string): boolean {
  if (
    !/^v?\d+\.\d+\.\d+(?:-[a-zA-Z0-9-]+(?:\.[a-zA-Z0-9-]+)*)?(?:\+[a-zA-Z0-9-]+(?:\.[a-zA-Z0-9-]+)*)?$/.test(
      value,
    )
  )
    return false;
  return value
    .replace(/^v/, '')
    .split(/[-+]/)[0]!
    .split('.')
    .every((part) => Number(part) <= 2147483647);
}

function asset(release: UpdateRelease, name: string): boolean {
  return (
    Array.isArray(release.assets) &&
    release.assets.some(
      (a) => a?.name === name && a.state === 'uploaded' && typeof a.size === 'number' && a.size > 0,
    )
  );
}

export function isDailyTag(tag: string): boolean {
  return /^daily-\d{4}-\d{2}-\d{2}(?:\.(?:[1-9][0-9]{0,2}))?$/.test(tag) ||
    /^\d+\.\d+\.\d+-daily\.d\d{8}(?:\.(?:[1-9][0-9]{0,2}))?$/.test(tag);
}

export function validNightlyMetadata(
  tag: string,
  metadata: Record<string, unknown> | undefined,
): boolean {
  if (!isDailyTag(tag) || !metadata || typeof metadata !== 'object') return false;
  if (!tag.startsWith('daily-')) {
    const match = tag.match(/^\d+\.\d+\.\d+-daily\.d(\d{4})(\d{2})(\d{2})(\.[1-9][0-9]{0,2})?$/);
    const source = metadata['source_commit'];
    if (metadata['tag'] !== tag || !match || typeof source !== 'string' || metadata['version'] !== `${tag}+${source.slice(0, 12)}`) return false;
    const legacy = `daily-${match[1]}-${match[2]}-${match[3]}${match[4] ?? ''}`;
    return validNightlyMetadata(legacy, { ...metadata, tag: legacy });
  }
  const [day, suffix] = tag.slice(6).split('.');
  const date = day!.replaceAll('-', '');
  const revision = suffix ? Number(suffix) : 1;
  if (revision > 999) return false;
  const versionSuffix = suffix ? `.${revision}` : '';
  const source = metadata['source_commit'];
  const version = metadata['version'];
  return (
    metadata['schema_version'] === 1 &&
    metadata['channel'] === 'nightly' &&
    metadata['tag'] === tag &&
    metadata['nightly_sequence'] === (suffix ? Number(date) * 1000 + revision : Number(date)) &&
    typeof source === 'string' &&
    /^[0-9a-f]{40}$/.test(source) &&
    typeof version === 'string' &&
    orderedVersion(version) &&
    /^\d+\.\d+\.\d+-daily\.d\d{8}(?:\.[0-9]+)?\+[0-9a-f]{12}$/.test(version) &&
    version.endsWith(`-daily.d${date}${versionSuffix}+${source.slice(0, 12)}`)
  );
}

export function apiNightlyMetadata(release: UpdateRelease): Record<string, unknown> | undefined {
  if (typeof release.body !== 'string') return undefined;
  const marker = '<!-- nightly-identity: ';
  const begin = release.body.indexOf(marker);
  if (begin < 0 || (begin > 0 && release.body[begin - 1] !== '\n') ||
      release.body.indexOf(marker, begin + marker.length) >= 0) return undefined;
  const end = release.body.indexOf(' -->', begin + marker.length);
  if (end < 0) return undefined;
  try {
    const metadata = JSON.parse(release.body.slice(begin + marker.length, end));
    return validNightlyMetadata(release.tag_name ?? '', metadata) ? metadata : undefined;
  } catch { return undefined; }
}

export function completeRelease(
  release: UpdateRelease,
  platform = 'windows',
  architecture = 'x86_64',
): boolean {
  if (
    !release ||
    release.draft !== false ||
    typeof release.prerelease !== 'boolean' ||
    typeof release.tag_name !== 'string'
  )
    return false;
  const packageAvailable =
    platform === 'windows'
      ? asset(release, 'VibepolloSetup.exe') ||
        (!isDailyTag(release.tag_name) &&
          (asset(release, `VibepolloSetup-v${release.tag_name.replace(/^v/, '')}.exe`) ||
            asset(release, `VibepolloSetup-${release.tag_name.replace(/^v/, '')}.exe`)))
      : platform === 'macos'
        ? architecture === 'aarch64'
          ? asset(release, 'Vibepollo-macOS-arm64.dmg')
          : architecture === 'x86_64' && asset(release, 'Vibepollo-macOS-x86_64.dmg')
        : platform === 'linux' &&
        Array.isArray(release.assets) &&
        release.assets.some(
          (a) =>
            typeof a.name === 'string' &&
            /(?:\.AppImage|\.deb|\.rpm|\.pkg\.tar\.zst)$/.test(a.name) &&
            (architecture === 'x86_64'
              ? /(?:x86_64|amd64)/.test(a.name)
              : architecture === 'aarch64' && /(?:aarch64|arm64)/.test(a.name)) &&
            asset(release, a.name),
        );
  return (
    !!packageAvailable &&
    (!isDailyTag(release.tag_name) ||
      (release.prerelease &&
        asset(release, 'SHA256SUMS')))
  );
}

function nightlyVersion(version: string): boolean {
  return version.includes('-daily.d');
}
function sameBase(left: string, right: string): boolean {
  return parseChangelogVersion(left).coreVersion === parseChangelogVersion(right).coreVersion;
}
function compareNotificationVersions(left: string, right: string): number {
  const a = nightlyVersion(left),
    b = nightlyVersion(right);
  if (sameBase(left, right) && a !== b) return a ? -1 : 1;
  return compareChangelogTags(left, right);
}

export function selectAvailableUpdate(
  installedVersion: string,
  releases: UpdateRelease[],
  notifyPrereleases: unknown,
  platform = 'windows',
  architecture = 'x86_64',
): ChangelogEntry | null {
  if (!orderedVersion(installedVersion)) return null;
  let best: ChangelogEntry | null = null;
  let bestIdentity = installedVersion;
  for (const release of releases) {
    if (
      !completeRelease(release, platform, architecture) ||
      (release.prerelease && !prereleasesEnabled(notifyPrereleases))
    )
      continue;
    let identity = release.tag_name!;
    let entry: ChangelogEntry | null;
    if (isDailyTag(identity)) {
      const metadata = apiNightlyMetadata(release);
      if (!metadata) continue;
      identity = metadata['version'] as string;
      entry = githubReleaseToChangelogEntry({ ...release, tag_name: identity });
      if (entry) entry.tag = release.tag_name!;
    } else {
      if (!orderedVersion(identity)) continue;
      entry = githubReleaseToChangelogEntry(release);
    }
    if (
      nightlyVersion(installedVersion) &&
      !nightlyVersion(identity) &&
      release.prerelease &&
      sameBase(installedVersion, identity)
    )
      continue;
    if (entry && compareNotificationVersions(identity, bestIdentity) > 0) {
      best = entry;
      bestIdentity = identity;
    }
  }
  return best;
}

export async function fetchUpdateReleases(
  signal: AbortSignal,
  platform = 'windows',
  allowPrereleases = true,
  architecture = 'x86_64',
): Promise<UpdateRelease[]> {
  const releases: UpdateRelease[] = [];
  let complete = false;
  for (let page = 1; page <= 10; page++) {
    const response = await fetch(
      `https://api.github.com/repos/Nonary/Vibepollo/releases?per_page=100&page=${page}`,
      { headers: { Accept: 'application/vnd.github+json' }, credentials: 'omit', signal },
    );
    if (!response.ok) throw new Error('releases-unavailable');
    const batch = await response.json();
    if (!Array.isArray(batch)) throw new Error('invalid-releases');
    releases.push(...batch);
    if (batch.length < 100) {
      complete = true;
      break;
    }
  }
  if (!complete) throw new Error('pagination-incomplete');
  signal.throwIfAborted();
  // Identity is already in the release API body. No metadata/installer requests.
  return releases;
}

const dismissedKey = 'vibepollo-dismissed-updates';
export function dismissedUpdate(storage: Pick<Storage, 'getItem'>, tag: string): boolean {
  try {
    const tags: unknown = JSON.parse(storage.getItem(dismissedKey) ?? '[]');
    return Array.isArray(tags) && tags.includes(tag);
  } catch {
    return false;
  }
}
export function dismissUpdate(storage: Pick<Storage, 'getItem' | 'setItem'>, tag: string): void {
  try {
    const value: unknown = JSON.parse(storage.getItem(dismissedKey) ?? '[]');
    const tags = Array.isArray(value) ? value.filter((v) => typeof v === 'string') : [];
    storage.setItem(dismissedKey, JSON.stringify([...new Set([...tags, tag])]));
  } catch {
    /* Storage may be disabled in a private browser. */
  }
}
