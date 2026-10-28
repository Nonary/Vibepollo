"""Notes from exact source commits of successfully published nightly releases."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path
from urllib.parse import quote

REPOSITORIES = {'Nonary/vibeshine', 'Nonary/Vibepollo', 'Nonary/moonlight-qt'}


def run(*args):
    return subprocess.check_output(args, text=True, encoding='utf-8').strip()


def exact_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{40}', value):
        raise ValueError('Expected an exact lowercase 40-character source SHA')
    return value


def sequence(repo, tag, version=None, source=None):
    if repo not in REPOSITORIES:
        raise ValueError('Repository is not in the nightly allowlist')
    if repo == 'Nonary/moonlight-qt':
        match = re.fullmatch(r'v6\.1\.0-daily\.([1-9][0-9]*)', tag)
        if not match or (version is not None and tag != 'v' + version):
            raise ValueError('Invalid Moonlight nightly identity')
        return int(match[1])
    nightly = re.fullmatch(r'([0-9]+\.[0-9]+\.[0-9]+)-nightly\.d(20[0-9]{2})([0-9]{2})([0-9]{2})\.([0-9a-f]{12})', tag)
    if nightly:
        import datetime
        datetime.date(*map(int, nightly.groups()[1:4]))
        if version is not None and (version != tag or nightly[5] != exact_sha(source)[:12]):
            raise ValueError('Nightly tag/version must match the exact source')
        return int(''.join(nightly.groups()[1:4]))
    semver = re.fullmatch(r'([0-9]+\.[0-9]+\.[0-9]+)-daily\.d(20[0-9]{2})([0-9]{2})([0-9]{2})(?:\.([1-9][0-9]{0,2}))?', tag)
    if semver:
        if version is not None and version != tag + '+' + exact_sha(source)[:12]:
            raise ValueError('SemVer nightly tag must equal its canonical version without build metadata')
        legacy = f'daily-{semver[2]}-{semver[3]}-{semver[4]}' + ('.' + semver[5] if semver[5] else '')
        return sequence(repo, legacy, version, source)
    match = re.fullmatch(r'daily-(20[0-9]{2})-([0-9]{2})-([0-9]{2})(?:\.([1-9][0-9]{0,2}))?', tag)
    if not match:
        raise ValueError('Invalid host nightly tag')
    import datetime
    datetime.date(*map(int, match.groups()[:3]))
    day = ''.join(match.groups()[:3])
    revision = int(match[4]) if match[4] else 1
    if revision > 999: raise ValueError('Nightly revision exceeds 999')
    suffix = r'\.' + str(revision) if match[4] else ''
    if version is not None and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+-daily\.d' + day + suffix + r'\+' + exact_sha(source)[:12], version):
        raise ValueError('Invalid host nightly version/source identity')
    return int(day) * 1000 + revision if match[4] else int(day)


def previous_release(repo, releases, metadata_reader, tag_resolver):
    """Published release state is the baseline; build runs never advance it."""
    candidates = []
    for release in releases:
        if release.get('draft') is not False or release.get('prerelease') is not True or not release.get('published_at'):
            continue
        tag = release.get('tag_name', '')
        try:
            sequence(repo, tag)
        except ValueError:
            continue
        candidates.append(release)
    for release in sorted(candidates, key=lambda r: (r['published_at'], r.get('id', 0)), reverse=True):
        tag = release['tag_name']
        assets = [a for a in release.get('assets', []) if a.get('name') == 'nightly-metadata.json']
        if assets:
            if len(assets) != 1:
                raise ValueError('Duplicate nightly metadata assets')
            metadata = metadata_reader(assets[0])
            if metadata.get('channel') != 'nightly':
                continue
            if metadata.get('schema_version') != 1 or metadata.get('tag') != tag:
                raise ValueError('Published nightly metadata identity mismatch')
            sha = exact_sha(metadata.get('source_commit'))
            if not isinstance(metadata.get('version'), str) or metadata.get('nightly_sequence') != sequence(repo, tag, metadata['version'], sha):
                raise ValueError('Published nightly version/sequence mismatch')
            if repo == 'Nonary/moonlight-qt' and 'windows_product_version' in metadata and metadata['windows_product_version'] != f'6.2.{metadata["nightly_sequence"]}':
                raise ValueError('Published nightly Windows product version mismatch')
            if tag_resolver(tag) != sha:
                raise ValueError('Published nightly tag does not match metadata source')
        else:
            # Migration: older published daily tags are authoritative exact targets.
            sha = exact_sha(tag_resolver(tag))
        return {'tag': tag, 'source_commit': sha}
    return None


def make_notes(repo, tag, version, source, previous, commits, ancestor=True):
    source = exact_sha(source)
    seq = sequence(repo, tag, version, source)
    first = previous is None
    old = exact_sha(previous['source_commit']) if previous else None
    compare = f'https://github.com/{repo}/compare/{old}...{source}' if old else None
    unchanged = old == source
    metadata = dict(schema_version=1, channel='nightly', tag=tag, version=version,
                    source_commit=source, nightly_sequence=seq,
                    previous_successful_tag=previous['tag'] if previous else None,
                    previous_source_commit=old, changelog_range=f'{old}..{source}' if old else None,
                    compare_url=compare, first_nightly=first, no_source_changes=unchanged,
                    commits_total=len(commits), commits_listed=len(commits))
    if repo == 'Nonary/moonlight-qt':
        metadata['windows_product_version'] = f'6.2.{seq}'
    product = {'Nonary/vibeshine': 'Vibeshine', 'Nonary/Vibepollo': 'Vibepollo', 'Nonary/moonlight-qt': 'Moonlight'}[repo]
    notes = [f'This is a nightly release of {product} and contains several bug fixes and may contain new features. Check the commit log for more information.', '',
             'Nightly Windows installers are unsigned and include self-signed gamepad components. They may cause false-positive antivirus alerts.', '',
             '<details>', '<summary>Commit log</summary>', '',
             f'Source: [{source[:12]}](https://github.com/{repo}/commit/{source})', '']
    if first:
        notes += ['First published nightly: no previous successful nightly baseline exists. No commit delta is claimed.']
    elif unchanged:
        notes += [f'No source changes since `{previous["tag"]}`; this nightly rebuilds the same source.']
    else:
        notes += [f'Since the preceding successfully published nightly `{previous["tag"]}`: [compare exact sources]({compare}).']
        if not ancestor:
            notes += ['', '**Warning: source history diverged or was rewound.** The list is the exact newly reachable commit set `previous_source_commit..source_commit`, not a forward ancestry claim.']
        if not commits:
            notes += ['', 'No newly reachable commits in this exact range. Source identity changed (for example, a rewind).']
    for sha, message in commits:
        exact_sha(sha)
        # Fence preserves the complete raw conventional commit message.
        fence = '`' * max(3, max((len(m[0]) for m in re.finditer(r'`+', message)), default=0) + 1)
        notes += ['', f'[{sha[:12]}](https://github.com/{repo}/commit/{sha})', fence + 'text', message.rstrip('\n'), fence]
    notes += ['', '</details>']
    return '\n'.join(notes) + '\n', metadata


def generate(repo, tag, version, source, assets, notes):
    exact_sha(source)
    sequence(repo, tag, version, source)
    if run('git', 'rev-parse', source + '^{commit}') != source:
        raise ValueError('Pinned source commit cannot be resolved exactly')
    pages = json.loads(run('gh', 'api', '--paginate', '--slurp', f'repos/{repo}/releases?per_page=100'))
    def resolve(tag):
        return exact_sha(run('git', 'rev-parse', 'refs/tags/' + tag + '^{commit}'))
    def read_metadata(asset):
        asset_id = asset.get('id')
        if not isinstance(asset_id, int) or asset_id <= 0:
            raise ValueError('Invalid metadata asset ID')
        return json.loads(run('gh', 'api', '-H', 'Accept: application/octet-stream', f'repos/{repo}/releases/assets/{asset_id}'))
    releases = [r for page in pages for r in page if r.get('tag_name') != tag]
    previous = previous_release(repo, releases, read_metadata, resolve)
    # The first nightly still needs an honest, useful commit log. A published
    # stable release supplies its initial baseline; drafts never do.
    if previous is None:
        stable = [r for r in releases if r.get('draft') is False and r.get('prerelease') is False and r.get('published_at')]
        if stable:
            baseline = max(stable, key=lambda r: (r['published_at'], r.get('id', 0)))
            previous = {'tag': baseline['tag_name'], 'source_commit': resolve(baseline['tag_name']), 'stable_baseline': True}
    commits, ancestor = [], True
    if previous:
        old = previous['source_commit']
        result = subprocess.run(['git', 'merge-base', '--is-ancestor', old, source], check=False)
        if result.returncode not in (0, 1):
            raise ValueError('Could not verify source ancestry')
        ancestor = result.returncode == 0
        # NUL framing preserves multiline bodies and avoids interpolated shell commands.
        raw = run('git', 'log', '--reverse', '--format=%H%x00%B%x00', old + '..' + source, '--')
        fields = raw.split('\0')
        for index in range(0, len(fields) - 1, 2):
            commits.append((exact_sha(fields[index].strip()), fields[index + 1]))
    body, metadata = make_notes(repo, tag, version, source, previous, commits, ancestor)
    if previous and previous.get('stable_baseline'):
        body = body.replace('preceding successfully published nightly', 'preceding stable release')
        metadata.update(first_nightly=True, previous_successful_tag=None, initial_stable_tag=previous['tag'])
    root = Path(assets)
    product = {'Nonary/vibeshine': 'Vibeshine', 'Nonary/Vibepollo': 'Vibepollo'}.get(repo)
    if product and (root / (product + 'Setup.exe')).is_file():
        downloads = [f'[Windows installer](https://github.com/{repo}/releases/download/{tag}/{product}Setup.exe)']
        packages = list(root.glob(product.lower() + '-*.pkg.tar.zst'))
        if len(packages) == 1:
            downloads.append(f'[Linux (Arch x86_64)](https://github.com/{repo}/releases/download/{tag}/{quote(packages[0].name)})')
        first_line, remainder = body.split('\n', 1)
        body = first_line + '\n\n' + ' · '.join(downloads) + '\n' + remainder
    # API-visible identity is generated only after the existing qualification gates.
    # The asset stays available for installed clients using the old contract.
    if repo != 'Nonary/moonlight-qt':
        evidence = root / 'daily-manifest.json'
        if evidence.is_file():
            metadata['qualification'] = json.loads(evidence.read_text(encoding='utf-8'))
    if '<!-- nightly-identity: ' in body:
        raise ValueError('Commit notes contain a reserved nightly identity marker')
    body = '<!-- nightly-identity: ' + json.dumps(metadata, separators=(',', ':')) + ' -->\n' + body
    (root / 'nightly-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
    Path(notes).write_text(body, encoding='utf-8')
    # Include metadata and existing manifest/provenance; never hash SHA256SUMS itself.
    files = sorted(p for p in root.iterdir() if p.name not in ('SHA256SUMS', 'daily-manifest.json', 'release-provenance.json'))
    if any(not p.is_file() or p.is_symlink() or '\n' in p.name or '\r' in p.name for p in files):
        raise ValueError('Unsafe checksum input')
    (root / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n' for p in files), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    for name in ('repo', 'tag', 'version', 'source', 'assets', 'notes'):
        parser.add_argument('--' + name, required=True)
    generate(**vars(parser.parse_args()))
