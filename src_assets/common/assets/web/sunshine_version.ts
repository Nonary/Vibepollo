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
  public preRelease: (string | number)[]; // semver prerelease identifiers

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
    this.preRelease = this.parsePreRelease(this.version);
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
    // Split out build metadata and prerelease but keep prerelease for separate parsing
    const plusIdx = v.indexOf('+');
    if (plusIdx >= 0) v = v.slice(0, plusIdx);
    const dashIdx = v.indexOf('-');
    const core = dashIdx >= 0 ? v.slice(0, dashIdx) : v;
    // Extract numeric major.minor.patch via regex to avoid NaN on suffixed parts
    const m = core.match(/^(\d+)\.(\d+)(?:\.(\d+))?$/);
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

  /** Parse prerelease identifiers (semver) as array of numbers/strings */
  parsePreRelease(version: string): (string | number)[] {
    if (!version) return [];
    let v = version.trim();
    if (v.startsWith('v') || v.startsWith('V')) v = v.slice(1);
    const plusIdx = v.indexOf('+');
    if (plusIdx >= 0) v = v.slice(0, plusIdx);
    const dashIdx = v.indexOf('-');
    if (dashIdx < 0) return [];
    const pre = v.slice(dashIdx + 1);
    if (!pre) return [];
    return pre.split('.').map((id) => {
      if (/^\d+$/.test(id)) {
        // numeric identifiers are compared numerically
        const n = Number(id);
        return Number.isFinite(n) ? n : id;
      }
      return id;
    });
  }

  /**
   * Return true if this version is greater than the other.
   */
  isGreater(otherVersion: SunshineVersion | string): boolean {
    const cmp = (a: SunshineVersion, b: SunshineVersion): number => {
      const [a0, a1, a2] = a.versionParts;
      const [b0, b1, b2] = b.versionParts;
      if (a0 !== b0) return a0 > b0 ? 1 : -1;
      if (a1 !== b1) return a1 > b1 ? 1 : -1;
      if (a2 !== b2) return a2 > b2 ? 1 : -1;
      const aPre = a.preRelease;
      const bPre = b.preRelease;
      if (aPre.length === 0 && bPre.length === 0) return 0; // equal
      if (aPre.length === 0) return 1; // release > prerelease
      if (bPre.length === 0) return -1; // prerelease < release
      const len = Math.max(aPre.length, bPre.length);
      for (let i = 0; i < len; i++) {
        const ai = aPre[i];
        const bi = bPre[i];
        if (ai === undefined) return -1; // shorter set has lower precedence
        if (bi === undefined) return 1;
        const aNum = typeof ai === 'number';
        const bNum = typeof bi === 'number';
        if (aNum && bNum) {
          if (ai !== bi) return (ai as number) > (bi as number) ? 1 : -1;
        } else if (aNum !== bNum) {
          // numeric identifiers always have lower precedence than non-numeric
          return aNum ? -1 : 1;
        } else {
          const as = String(ai);
          const bs = String(bi);
          if (as !== bs) return as > bs ? 1 : -1;
        }
      }
      return 0;
    };

    const other =
      otherVersion instanceof SunshineVersion ? otherVersion : new SunshineVersion(otherVersion);
    return cmp(this, other) > 0;
  }
}
