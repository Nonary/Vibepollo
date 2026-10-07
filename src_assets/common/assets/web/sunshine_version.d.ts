export interface GitHubRelease {
  tag_name: string;
  name: string;
  html_url: string;
  body: string;
  prerelease?: boolean;
  [key: string]: any;
}

export default class SunshineVersion {
  version: string;
  versionParts: [number, number, number];
  versionMajor: number;
  versionMinor: number;
  versionPatch: number;
  preRelease: (string | number)[];
  constructor(version: string);
  static fromRelease(release: GitHubRelease): SunshineVersion;
  isGreaterRelease(release: GitHubRelease | string): boolean;
  parseVersion(version: string): [number, number, number];
  parsePreRelease(version: string): (string | number)[];
  isGreater(otherVersion: SunshineVersion | string): boolean;
}
