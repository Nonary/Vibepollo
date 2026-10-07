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

export declare function normalizeChangelogTag(tag: string): string;
export declare function parseChangelogVersion(tag: string): VersionInfo;
export declare function compareChangelogTags(aTag: string, bTag: string): number;
export declare function sortChangelogEntries(entries: ChangelogEntry[]): ChangelogEntry[];
export declare function parseMarkdownSections(body: string): ChangelogSection[];
export declare function parseBundledReleaseNote(
  filename: string,
  content: string,
): ChangelogEntry | null;
export declare function githubReleaseToChangelogEntry(
  release: GitHubReleaseLike,
): ChangelogEntry | null;
export declare function mergeChangelogEntries(
  bundled: ChangelogEntry[],
  github: ChangelogEntry[],
): ChangelogEntry[];
