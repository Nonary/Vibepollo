import {
  githubReleaseToChangelogEntry,
  mergeChangelogEntries,
  parseBundledReleaseNote,
  parseChangelogVersion,
  sortChangelogEntries,
  type ChangelogEntry,
} from '@web/utils/changelog';

function entry(tag: string): ChangelogEntry {
  const parsed = parseBundledReleaseNote(
    `${tag}.md`,
    `# Vibepollo ${tag} - 2026-05-01\n\n## Fixes\n- Fixed ${tag}`,
  );
  if (!parsed) throw new Error(`Failed to parse ${tag}`);
  return parsed;
}

describe('changelog utilities', () => {
  test('parses bundled release-note filename and markdown content', () => {
    const release = parseBundledReleaseNote(
      '1.15.4-stable.2.md',
      '# Vibepollo 1.15.4-stable.2 - 2026-05-04\n\nNotice text.\n\n## Fixes\n- Fixed Playnite autosync.\n- Fixed HDR detection.',
    );

    expect(release).not.toBeNull();
    expect(release?.tag).toBe('1.15.4-stable.2');
    expect(release?.date).toBe('2026-05-04');
    expect(release?.coreVersion).toBe('1.15.4');
    expect(release?.releaseLine).toBe('1.15');
    expect(release?.channel).toBe('stable');
    expect(release?.sections.map((section) => section.heading)).toEqual(['Notes', 'Fixes']);
    expect(release?.sections[1]?.bullets).toEqual(['Fixed Playnite autosync.', 'Fixed HDR detection.']);
  });

  test('groups base releases and stable incrementals by numeric core and release line', () => {
    const base = parseChangelogVersion('1.15.4');
    const stable1 = parseChangelogVersion('1.15.4-stable.1');
    const stable2 = parseChangelogVersion('1.15.4-stable.2');
    const nextPatch = parseChangelogVersion('1.15.5-alpha.1');

    expect([base.coreVersion, stable1.coreVersion, stable2.coreVersion]).toEqual([
      '1.15.4',
      '1.15.4',
      '1.15.4',
    ]);
    expect([base.releaseLine, stable1.releaseLine, stable2.releaseLine, nextPatch.releaseLine]).toEqual([
      '1.15',
      '1.15',
      '1.15',
      '1.15',
    ]);
    expect(nextPatch.coreVersion).toBe('1.15.5');
  });

  test('merges GitHub releases over bundled notes for matching tags', () => {
    const bundled = [entry('1.15.4')];
    const githubEntry = githubReleaseToChangelogEntry({
      tag_name: 'v1.15.4',
      name: 'v1.15.4 GitHub release',
      body: '## Changes\n- GitHub body wins.',
      html_url: 'https://github.com/Nonary/Vibepollo/releases/tag/1.15.4',
      published_at: '2026-05-03T00:00:00Z',
      prerelease: false,
    });

    expect(githubEntry).not.toBeNull();
    const merged = mergeChangelogEntries(bundled, githubEntry ? [githubEntry] : []);

    expect(merged).toHaveLength(1);
    expect(merged[0]?.source).toBe('github');
    expect(merged[0]?.name).toBe('v1.15.4 GitHub release');
    expect(merged[0]?.body).toContain('GitHub body wins');
    expect(merged[0]?.url).toContain('github.com/Nonary/Vibepollo');
  });

  test('sorts stable respins after their base release within the same core', () => {
    const sorted = sortChangelogEntries([
      entry('1.15.4-stable.2'),
      entry('1.15.5-alpha.1'),
      entry('1.15.4'),
      entry('1.15.4-stable.1'),
    ]).map((release) => release.tag);

    expect(sorted).toEqual(['1.15.5-alpha.1', '1.15.4', '1.15.4-stable.1', '1.15.4-stable.2']);
  });
});
