export type ChangelogSource = 'bundled' | 'github';
export type ChangelogChannel = 'stable' | 'alpha' | 'beta' | 'rc' | 'other';

export interface ChangelogSection {
  heading: string;
  bullets: string[];
  body: string[];
}

export interface ChangelogEntry {
  tag: string;
  name: string;
  date: string;
  body: string;
  sections: ChangelogSection[];
  coreVersion: string;
  releaseLine: string;
  channel: ChangelogChannel;
  source: ChangelogSource;
  url?: string;
  prerelease?: boolean;
}

export interface BundledChangelogAsset {
  generatedAt: string;
  releases: ChangelogEntry[];
}

export interface GitHubReleaseLike {
  tag_name?: string;
  name?: string | null;
  body?: string | null;
  html_url?: string | null;
  published_at?: string | null;
  created_at?: string | null;
  prerelease?: boolean;
  draft?: boolean;
}

interface VersionInfo {
  normalizedTag: string;
  numeric: [number, number, number];
  preRelease: (string | number)[];
  coreVersion: string;
  releaseLine: string;
  channel: ChangelogChannel;
}

function stripTagPrefix(tag: string): string {
  const trimmed = (tag || '').trim();
  return trimmed.startsWith('v') || trimmed.startsWith('V') ? trimmed.slice(1) : trimmed;
}

export function normalizeChangelogTag(tag: string): string {
  return stripTagPrefix(tag).replace(/\.md$/i, '');
}

export function parseChangelogVersion(tag: string): VersionInfo {
  const normalizedTag = normalizeChangelogTag(tag);
  const withoutBuild = normalizedTag.split('+')[0] ?? normalizedTag;
  const [coreRaw = '0.0.0', preRaw = ''] = withoutBuild.split(/-(.+)/);
  const match = coreRaw.match(/^(\d+)\.(\d+)(?:\.(\d+))?/);
  const major = match?.[1] ? Number(match[1]) : 0;
  const minor = match?.[2] ? Number(match[2]) : 0;
  const patch = match?.[3] ? Number(match[3]) : 0;
  const preRelease = preRaw
    ? preRaw.split('.').map((part) => (/^\d+$/.test(part) ? Number(part) : part))
    : [];
  const first = String(preRelease[0] ?? '').toLowerCase();
  let channel: ChangelogChannel = 'stable';
  if (first === 'alpha' || first === 'beta' || first === 'rc' || first === 'stable') {
    channel = first === 'stable' ? 'stable' : first;
  } else if (first) {
    channel = 'other';
  }
  return {
    normalizedTag,
    numeric: [major, minor, patch],
    preRelease,
    coreVersion: `${major}.${minor}.${patch}`,
    releaseLine: `${major}.${minor}`,
    channel,
  };
}

function compareIdentifiers(
  aIdentifiers: (string | number)[],
  bIdentifiers: (string | number)[],
  startIndex = 0,
): number {
  const len = Math.max(aIdentifiers.length, bIdentifiers.length);
  for (let i = startIndex; i < len; i++) {
    const ai = aIdentifiers[i];
    const bi = bIdentifiers[i];
    if (ai === undefined) return -1;
    if (bi === undefined) return 1;
    const aNum = typeof ai === 'number';
    const bNum = typeof bi === 'number';
    if (aNum && bNum) {
      if (ai !== bi) return ai > bi ? 1 : -1;
    } else if (aNum !== bNum) {
      return aNum ? -1 : 1;
    } else {
      const as = String(ai);
      const bs = String(bi);
      if (as !== bs) return as > bs ? 1 : -1;
    }
  }
  return 0;
}

function hasStableChannel(preRelease: (string | number)[]): boolean {
  return String(preRelease[0] ?? '').toLowerCase() === 'stable';
}

export function compareChangelogTags(aTag: string, bTag: string): number {
  const a = parseChangelogVersion(aTag);
  const b = parseChangelogVersion(bTag);
  for (let i = 0; i < 3; i++) {
    const av = a.numeric[i] ?? 0;
    const bv = b.numeric[i] ?? 0;
    if (av !== bv) return av > bv ? 1 : -1;
  }

  const aStable = hasStableChannel(a.preRelease);
  const bStable = hasStableChannel(b.preRelease);
  if (aStable && bStable) return compareIdentifiers(a.preRelease, b.preRelease, 1);
  if (aStable !== bStable) return aStable ? 1 : -1;

  if (a.preRelease.length === 0 && b.preRelease.length === 0) return 0;
  if (a.preRelease.length === 0) return 1;
  if (b.preRelease.length === 0) return -1;
  return compareIdentifiers(a.preRelease, b.preRelease);
}

function stableRespinIndex(entry: Pick<ChangelogEntry, 'tag'>): number {
  const info = parseChangelogVersion(entry.tag);
  const first = String(info.preRelease[0] ?? '').toLowerCase();
  if (info.preRelease.length === 0) return 0;
  if (first === 'stable') {
    const id = info.preRelease[1];
    return typeof id === 'number' ? id : 1;
  }
  return -1;
}

export function sortChangelogEntries(entries: ChangelogEntry[]): ChangelogEntry[] {
  return [...entries].sort((a, b) => {
    const aInfo = parseChangelogVersion(a.tag);
    const bInfo = parseChangelogVersion(b.tag);
    if (aInfo.coreVersion === bInfo.coreVersion) {
      const aStableOrder = stableRespinIndex(a);
      const bStableOrder = stableRespinIndex(b);
      if (aStableOrder >= 0 && bStableOrder >= 0 && aStableOrder !== bStableOrder) {
        return aStableOrder - bStableOrder;
      }
      return -compareChangelogTags(a.tag, b.tag);
    }
    const cmp = compareChangelogTags(a.tag, b.tag);
    if (cmp !== 0) return -cmp;
    return (b.date || '').localeCompare(a.date || '');
  });
}

export function parseMarkdownSections(body: string): ChangelogSection[] {
  const sections: ChangelogSection[] = [];
  let current: ChangelogSection | null = null;

  for (const rawLine of (body || '').replace(/\r\n/g, '\n').split('\n')) {
    const line = rawLine.trimEnd();
    const heading = line.match(/^##+\s+(.+)$/);
    if (heading?.[1]) {
      current = { heading: heading[1].trim(), bullets: [], body: [] };
      sections.push(current);
      continue;
    }

    if (!current) {
      if (line.trim()) {
        current = { heading: 'Notes', bullets: [], body: [] };
        sections.push(current);
      } else {
        continue;
      }
    }

    const bullet = line.match(/^[-*]\s+(.+)$/);
    if (bullet?.[1]) {
      current.bullets.push(bullet[1].trim());
    } else if (line.trim()) {
      current.body.push(line.trim());
    }
  }

  return sections;
}

export function parseBundledReleaseNote(filename: string, content: string): ChangelogEntry | null {
  const tag = normalizeChangelogTag(filename.replace(/^.*[\\/]/, ''));
  if (!/^\d+\.\d+(?:\.\d+)?(?:[-+].*)?$/i.test(tag)) return null;
  const info = parseChangelogVersion(tag);
  const normalizedBody = (content || '').replace(/\r\n/g, '\n').trim();
  const lines = normalizedBody.split('\n');
  let name = `Vibepollo ${tag}`;
  let date = '';
  let body = normalizedBody;
  const title = lines[0]?.match(/^#\s+(.+?)(?:\s+-\s+(\d{4}-\d{2}-\d{2}))?\s*$/);
  if (title) {
    name = title[1]?.trim() || name;
    date = title[2] ?? '';
    body = lines.slice(1).join('\n').trim();
  }
  return {
    tag,
    name,
    date,
    body,
    sections: parseMarkdownSections(body),
    coreVersion: info.coreVersion,
    releaseLine: info.releaseLine,
    channel: info.channel,
    source: 'bundled',
    prerelease: info.channel !== 'stable',
  };
}

export function githubReleaseToChangelogEntry(release: GitHubReleaseLike): ChangelogEntry | null {
  if (!release || release.draft || !release.tag_name) return null;
  const tag = normalizeChangelogTag(release.tag_name);
  if (!/^\d+\.\d+(?:\.\d+)?(?:[-+].*)?$/i.test(tag)) return null;
  const info = parseChangelogVersion(tag);
  const body = (release.body ?? '').trim();
  const entry: ChangelogEntry = {
    tag,
    name: (release.name || `Vibepollo ${tag}`).trim(),
    date: (release.published_at || release.created_at || '').slice(0, 10),
    body,
    sections: parseMarkdownSections(body),
    coreVersion: info.coreVersion,
    releaseLine: info.releaseLine,
    channel: info.channel,
    source: 'github',
    prerelease: release.prerelease ?? info.channel !== 'stable',
  };
  if (release.html_url) entry.url = release.html_url;
  return entry;
}

export function mergeChangelogEntries(
  bundled: ChangelogEntry[],
  github: ChangelogEntry[],
): ChangelogEntry[] {
  const byTag = new Map<string, ChangelogEntry>();
  for (const entry of bundled) {
    byTag.set(normalizeChangelogTag(entry.tag).toLowerCase(), entry);
  }
  for (const entry of github) {
    const key = normalizeChangelogTag(entry.tag).toLowerCase();
    const existing = byTag.get(key);
    if (!existing) {
      byTag.set(key, entry);
      continue;
    }
    const body = entry.body || existing.body;
    byTag.set(key, {
      ...existing,
      ...entry,
      body,
      sections: parseMarkdownSections(body),
      source: 'github',
      name: entry.name || existing.name,
      date: entry.date || existing.date,
      ...(entry.url || existing.url ? { url: entry.url || existing.url } : {}),
    });
  }
  return sortChangelogEntries(Array.from(byTag.values()));
}
