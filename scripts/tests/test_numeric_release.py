import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("numeric_release", Path(__file__).resolve().parents[1] / "numeric_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class NumericReleaseTests(unittest.TestCase):
    def test_patch_counts_all_fixes_from_previous_release(self):
        messages = [f"fix: repair issue {number}" for number in range(10)]
        self.assertEqual(release.allocate_version("2.0.6", messages), "2.0.16")

    def test_each_feature_advances_minor_and_discards_fix_count(self):
        fixes = [f"fix: repair issue {number}" for number in range(10)]
        self.assertEqual(release.allocate_version("2.0.6", ["feat: add one", *fixes]), "2.1.0")
        self.assertEqual(release.allocate_version("2.0.6", ["feat: add one", "feat(ui): add two", *fixes]), "2.2.0")

    def test_conventional_features_and_fixes_only(self):
        messages = ["feat!: add a feature", "fix(core)!: repair a regression", "docs: update docs", "chore: tidy"]
        self.assertEqual(release.conventional_counts(messages), {"feat": 1, "fix": 1})
        self.assertEqual(release.allocate_version("2.0.6", messages), "2.1.0")

    def test_no_eligible_commits_does_not_allocate(self):
        with self.assertRaisesRegex(ValueError, "No eligible conventional feat/fix"):
            release.allocate_version("2.0.6", ["docs: clarify setup", "Merge pull request #1"])

    def test_patch_overflow_fails_closed_without_normalizing(self):
        with self.assertRaisesRegex(ValueError, "supported Windows packaging range"):
            release.allocate_version("2.0.654", ["fix: patch overflow"])

    def test_feature_minor_overflow_fails_closed(self):
        with self.assertRaisesRegex(ValueError, "supported Windows packaging range"):
            release.allocate_version("2.255.6", ["feat: minor overflow"])

    def test_same_numeric_version_is_ambiguous_across_channels(self):
        releases = [[
            {"tag_name": "2.0.6", "draft": False, "published_at": "2026-11-01", "prerelease": False},
            {"tag_name": "v2.0.6", "draft": False, "published_at": "2026-11-02", "prerelease": True},
        ]]
        with self.assertRaisesRegex(ValueError, "Ambiguous published release ordering"):
            release.latest_published_base(releases)

    def test_candidates_must_outrank_legacy_and_prefixed_tags(self):
        with self.assertRaisesRegex(ValueError, "not newer"):
            release.require_newer_than("2.0.7", ["2.0.7-stable.2"])
        with self.assertRaisesRegex(ValueError, "not newer"):
            release.require_newer_than("2.0.7", ["v2.0.7"])
        release.require_newer_than("2.0.8", ["2.0.7-stable.2", "2.0.7-nightly.d20261001.abc123def456"])

    def test_unrelated_date_style_upstream_tags_are_not_product_versions(self):
        imported = ["v2025.814.190855", "v2026.221.41532", "webrtc-win-x64-v2-db3ba7af86dd-af1b957f0cf8-a20a4ee3232e"]
        release.require_newer_than("2.0.7", imported)
        self.assertIsNone(release.version_key("v2026.221.41532"))

    def test_published_base_uses_highest_channel_version(self):
        releases = [[
            {"tag_name": "2.0.6", "draft": False, "published_at": "2026-11-01", "prerelease": False},
            {"tag_name": "2.0.7-nightly.d20261102.r.1.abcdefabcdef", "draft": False,
             "published_at": "2026-11-02", "prerelease": True},
        ]]
        self.assertEqual(release.latest_published_base(releases)[0], (2, 0, 7))

    def test_merge_of_independently_published_sources_counts_only_new_commits(self):
        with tempfile.TemporaryDirectory() as temp:
            def git(*args):
                return subprocess.run(["git", *args], cwd=temp, check=True, capture_output=True, text=True).stdout.strip()

            git("init", "-q", "-b", "master")
            git("config", "user.name", "Release Test")
            git("config", "user.email", "release-test@example.invalid")
            Path(temp, "file").write_text("base\n")
            git("add", "file")
            git("commit", "-qm", "chore: base")
            git("branch", "beta")
            git("checkout", "-q", "beta")
            Path(temp, "beta").write_text("published beta\n")
            git("add", "beta")
            git("commit", "-qm", "fix: beta change")
            beta_source = git("rev-parse", "HEAD")
            git("checkout", "-q", "master")
            Path(temp, "stable").write_text("published stable\n")
            git("add", "stable")
            git("commit", "-qm", "fix: stable change")
            stable_source = git("rev-parse", "HEAD")
            git("merge", "--no-ff", "beta", "-m", "Merge published beta")
            Path(temp, "new").write_text("new\n")
            git("add", "new")
            git("commit", "-qm", "fix: new change")
            target = git("rev-parse", "HEAD")
            original_cwd = os.getcwd()
            try:
                os.chdir(temp)
                messages = release.commit_messages_since_sources(target, [beta_source, stable_source])
            finally:
                os.chdir(original_cwd)
            self.assertEqual(release.conventional_counts(messages), {"feat": 0, "fix": 1})
            self.assertIn("fix: new change", messages)


if __name__ == "__main__":
    unittest.main()
