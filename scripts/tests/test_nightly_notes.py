import importlib.util
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('nightly_notes', Path(__file__).parents[1] / 'nightly_notes.py')
n = importlib.util.module_from_spec(spec)
spec.loader.exec_module(n)
A, B = 'a' * 40, 'b' * 40
REPO = 'Nonary/vibeshine'


def release(tag='daily-2026-10-10', **kwargs):
    return dict(tag_name=tag, draft=False, prerelease=True, published_at='2026-10-10T12:00:00Z', assets=[], **kwargs)


class NightlyNotesTests(unittest.TestCase):
    def notes(self, previous=None, commits=(), ancestor=True):
        return n.make_notes(REPO, 'daily-2026-10-11', '1.2.3-daily.d20261011+' + B[:12], B, previous, list(commits), ancestor)

    def test_published_only_baseline_ignores_draft_stable_other_channel(self):
        good = release()
        draft = dict(good, tag_name='daily-2026-10-11', draft=True, published_at=None)
        stable = dict(good, tag_name='daily-2026-10-12', prerelease=False)
        manual = dict(good, tag_name='1.2.3-vrr.9')
        failed = dict(good, tag_name='daily-2026-10-13', draft=True)
        found = n.previous_release(REPO, [draft, stable, failed, manual, good], lambda a: {}, lambda tag: A)
        self.assertEqual(found, {'tag': good['tag_name'], 'source_commit': A})

    def test_first_nightly_claims_no_delta(self):
        notes, data = self.notes()
        self.assertIn('no previous successful nightly baseline', notes)
        self.assertTrue(data['first_nightly'])
        self.assertIsNone(data['changelog_range'])
        self.assertIsNone(data['compare_url'])
        self.assertEqual(data['commits_total'], 0)

    def test_same_source_rebuild(self):
        notes, data = self.notes({'tag': 'daily-2026-10-10', 'source_commit': B})
        self.assertIn('No source changes', notes)
        self.assertTrue(data['no_source_changes'])
        self.assertFalse(data['first_nightly'])

    def test_divergence_exact_range_and_links_preserve_raw_message(self):
        raw = 'fix(stream)!: correct pacing\n\nBREAKING CHANGE: old pacing removed'
        notes, data = self.notes({'tag': 'daily-2026-10-10', 'source_commit': A}, [(B, raw)], False)
        self.assertIn('history diverged or was rewound', notes)
        self.assertIn(raw, notes)
        self.assertIn(f'https://github.com/{REPO}/commit/{B}', notes)
        self.assertEqual(data['changelog_range'], A + '..' + B)
        self.assertEqual(data['compare_url'], f'https://github.com/{REPO}/compare/{A}...{B}')
        self.assertEqual(data['commits_listed'], 1)

    def test_rewind_empty_delta_is_not_no_source_changes(self):
        notes, data = self.notes({'tag': 'daily-2026-10-10', 'source_commit': A}, [], False)
        self.assertIn('No newly reachable commits', notes)
        self.assertFalse(data['no_source_changes'])

    def test_other_metadata_channel_skipped(self):
        newer = dict(release('daily-2026-10-11'), published_at='2026-10-11T12:00:00Z', assets=[{'name': 'nightly-metadata.json'}])
        found = n.previous_release(REPO, [release(), newer], lambda a: {'channel': 'stable'}, lambda tag: A)
        self.assertEqual(found['tag'], 'daily-2026-10-10')

    def test_metadata_source_must_match_immutable_tag(self):
        r = dict(release(), assets=[{'name': 'nightly-metadata.json'}])
        data = dict(schema_version=1, channel='nightly', tag=r['tag_name'], source_commit=B, nightly_sequence=20261010, version='1.2.3-daily.d20261010+' + B[:12])
        with self.assertRaisesRegex(ValueError, 'tag does not match'):
            n.previous_release(REPO, [r], lambda a: data, lambda tag: A)

    def test_allowlist_exact_shas_and_no_injection(self):
        for value in ('main', A + '\necho bad', A[:12], '--all'):
            with self.assertRaises(ValueError):
                n.exact_sha(value)
        with self.assertRaises(ValueError):
            n.sequence('attacker/repo', 'daily-2026-10-11')
        with self.assertRaises(ValueError):
            n.sequence(REPO, 'daily-2026-10-11\necho bad')

    def test_moonlight_contract(self):
        notes, data = n.make_notes('Nonary/moonlight-qt', 'v6.1.0-daily.256', '6.1.0-daily.256', B, None, [])
        self.assertEqual(data['nightly_sequence'], 256)
        self.assertEqual(data['channel'], 'nightly')

    def test_exact_delta_argv_preserves_multiline_messages(self):
        import tempfile
        import json
        calls = []
        def fake_run(*args):
            calls.append(args)
            if args[0] == 'gh':
                return json.dumps([[release()]])
            if args[1] == 'log':
                return B + '\0fix: first\n\nbody\0\n' + A + '\0docs: second\n\0'
            if 'refs/tags/' in args[-1]:
                return A
            return B
        with tempfile.TemporaryDirectory() as temp:
            notes = Path(temp) / 'notes.md'
            assets = Path(temp) / 'assets'
            assets.mkdir()
            (assets / 'app.exe').write_bytes(b'app')
            with patch.object(n, 'run', fake_run), patch.object(n.subprocess, 'run') as process:
                process.return_value.returncode = 1
                n.generate(REPO, 'daily-2026-10-11', '1.2.3-daily.d20261011+' + B[:12], B, str(assets), str(notes))
            self.assertIn(('git', 'log', '--reverse', '--format=%H%x00%B%x00', A + '..' + B, '--'), calls)
            self.assertIn('fix: first\n\nbody', notes.read_text())
            self.assertEqual(json.loads((assets / 'nightly-metadata.json').read_text())['commits_total'], 2)

    def test_pagination_and_checksum_metadata(self):
        import tempfile
        calls = []
        def fake_run(*args):
            calls.append(args)
            if args[0] == 'gh':
                return '[[], []]'
            return B
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'app.exe').write_bytes(b'artifact')
            (root / 'SHA256SUMS').write_text('old')
            with patch.object(n, 'run', fake_run):
                n.generate(REPO, 'daily-2026-10-11', '1.2.3-daily.d20261011+' + B[:12], B, temp, str(root.parent / (root.name + '-notes.md')))
            sums = (root / 'SHA256SUMS').read_text()
            self.assertIn('nightly-metadata.json', sums)
            self.assertNotIn('SHA256SUMS', sums)
            self.assertIn(('gh', 'api', '--paginate', '--slurp', f'repos/{REPO}/releases?per_page=100'), calls)
            (root.parent / (root.name + '-notes.md')).unlink()


if __name__ == '__main__':
    unittest.main()
