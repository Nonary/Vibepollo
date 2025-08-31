// Minimal GitHub release shape used by the UI and SunshineVersion.
// Keep only the fields we actually reference in the code to reduce noise.
export interface GitHubRelease {
  tag_name: string;
  name: string;
  html_url: string;
  body: string;
  prerelease?: boolean;
  [key: string]: any;
}

export default class SunshineVersion {
  public version: string;
  public versionParts: [number, number, number];
  public versionMajor: number;
  public versionMinor: number;
  public versionPatch: number;

  /**
   * Construct a SunshineVersion. Either pass a GitHubRelease or a version string.
   * All fields on the instance are non-nullable and initialised to sensible defaults.
   */
  constructor(version: string) {
    this.version = version || '0.0.0';
    this.versionParts = this.parseVersion(this.version);
    this.versionMajor = this.versionParts[0];
    this.versionMinor = this.versionParts[1];
    this.versionPatch = this.versionParts[2];
  }

  /** Create a SunshineVersion from a GitHubRelease */
  static fromRelease(release: GitHubRelease): SunshineVersion {
    const tag = (release && release.tag_name) || '0.0.0';
    return new SunshineVersion(tag);
  }

  /** Compare this version to a release directly */
  isGreaterRelease(release: GitHubRelease | string): boolean {
    if (typeof release === 'string') return this.isGreater(release);
    const tag = release.tag_name || '';
    return this.isGreater(tag);
  }

  /**
   * Parse a version string like "v1.2.3" or "1.2" into a 3-number array.
   * Always returns a length-3 array of numbers (no nulls).
   */
  parseVersion(version: string): [number, number, number] {
    if (!version) return [0, 0, 0];
    let v = version.trim();
    // Strip leading 'v'
    if (v.startsWith('v') || v.startsWith('V')) {
      v = v.slice(1);
    }
    // Drop pre-release/build metadata (e.g., -rc.1, +build)
    const dash = v.indexOf('-');
    const plus = v.indexOf('+');
    const cutIdx = [dash, plus].filter((i) => i >= 0).sort((a, b) => a - b)[0];
    if (cutIdx !== undefined) {
      v = v.slice(0, cutIdx);
    }
    // Extract numeric major.minor.patch via regex to avoid NaN on suffixed parts
    const m = v.match(/^(\d+)\.(\d+)(?:\.(\d+))?$/);
    if (m) {
      const maj = parseInt(m[1]!, 10);
      const min = parseInt(m[2]!, 10);
      const pat = m[3] ? parseInt(m[3]!, 10) : 0;
      return [maj, min, pat];
    }
    // Fallback: split and coerce numerics defensively
    const parts = v.split('.').map((p) => {
      const n = parseInt(p, 10);
      return Number.isFinite(n) ? n : 0;
    });
    while (parts.length < 3) parts.push(0);
    const tup = parts.slice(0, 3) as [number, number, number];
    return tup;
  }

  /**
   * Return true if this version is greater than the other.
   */
  isGreater(otherVersion: SunshineVersion | string): boolean {
    let otherVersionParts: [number, number, number];
    if (otherVersion instanceof SunshineVersion) {
      otherVersionParts = otherVersion.versionParts;
    } else if (typeof otherVersion === 'string') {
      otherVersionParts = this.parseVersion(otherVersion);
    } else {
      throw new Error(
        'Invalid argument: otherVersion must be a SunshineVersion object or a version string',
      );
    }

    const [a0, a1, a2] = this.versionParts;
    const [b0, b1, b2] = otherVersionParts;
    if (a0 !== b0) return a0 > b0;
    if (a1 !== b1) return a1 > b1;
    return a2 > b2;
  }
}
