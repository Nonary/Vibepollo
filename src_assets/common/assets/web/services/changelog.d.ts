import type { ChangelogEntry } from '../utils/changelog';

export interface LoadChangelogResult {
  releases: ChangelogEntry[];
  bundledOnly: boolean;
  githubError: string | null;
  latestAvailable: ChangelogEntry | null;
  installedVersion: string;
}

export declare function getInstalledVersion(): Promise<string>;
export declare function loadBundledChangelog(): Promise<ChangelogEntry[]>;
export declare function loadGithubChangelog(): Promise<ChangelogEntry[]>;
export declare function loadChangelog(): Promise<LoadChangelogResult>;
