"""Daily resource versions retain one real release-tag anchor across HEADs."""
from pathlib import Path
import re
import subprocess
import tempfile
import time
import unittest

SCRIPT = Path(__file__).resolve().parents[4] / 'cmake/prep/emit_windows_versioninfo.cmake'


class DailyVersionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='daily-version-contract-')
        cls.root = Path(cls.temp.name)
        def git(*args):
            return subprocess.check_output(['git', '-c', 'user.name=Version contract', '-c', 'user.email=version-contract@example.invalid', *args], cwd=cls.root, text=True).strip()
        cls.git = staticmethod(git)
        git('init', '--quiet')
        (cls.root / 'fixture').write_text('release\n')
        git('add', 'fixture')
        git('commit', '--quiet', '-m', 'release fixture')
        git('tag', '2.0.0')
        cls.anchor = int(git('log', '-1', '--format=%ct'))
        # Use actual commit timestamps, never forged author/committer dates.
        time.sleep(2)
        (cls.root / 'fixture').write_text('later commit\n')
        git('commit', '--quiet', '-am', 'later fixture')
        assert int(git('log', '-1', '--format=%ct')) > cls.anchor

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def emit(self, name, elapsed, version='2.0.0-daily.d20261011+fixture', patch=0):
        output = self.root / (name + '.h')
        result = subprocess.run(['cmake', f'-DOUTPUT_FILE={output}', f'-DSOURCE_DIR={self.root}',
                                 '-DPROJECT_VERSION_MAJOR=2', '-DPROJECT_VERSION_MINOR=0', f'-DPROJECT_VERSION_PATCH={patch}',
                                 f'-DPROJECT_VERSION_FULL={version}', f'-DTEST_SOURCE_TIME_EPOCH={self.anchor + elapsed}',
                                 '-P', str(SCRIPT)], text=True, capture_output=True)
        return result, output

    def test_new_head_and_following_dates_keep_release_anchor(self):
        for name, elapsed, version, expected in [('today', 601, '2.0.0-daily.d20261011+first', 6),
                                                 ('tomorrow', 86401, '2.0.0-daily.d20261012+second', 721)]:
            result, output = self.emit(name, elapsed, version)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f'anchor={self.anchor}', result.stdout)
            self.assertEqual(int(re.search(r'RC_VERSION_REVISION (\d+)', output.read_text())[1]), expected)

    def test_missing_matching_stable_anchor_fails_closed(self):
        result, _ = self.emit('missing', 601, '2.0.1-daily.d20261011+fixture', patch=1)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('requires a reachable', ' '.join(result.stderr.split()))

    def test_clock_rollback_still_rejected(self):
        self.assertEqual(self.emit('rollback', 601)[0].returncode, 0)
        result, _ = self.emit('rollback', 480)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('older than cached revision', ' '.join(result.stderr.split()))

    def test_revision_overflow_still_rejected(self):
        result, _ = self.emit('overflow', 65534 * 120)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('exceeds 65534', result.stderr)


if __name__ == '__main__':
    unittest.main()
