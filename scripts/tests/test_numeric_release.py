import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "numeric_release", Path(__file__).resolve().parents[1] / "numeric_release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)

BASE = "b" * 40
SOURCE = "a" * 40
STABLE = {"tag_name": "2.0.6", "draft": False, "prerelease": False,
          "published_at": "2026-11-01T00:00:00Z"}
RELEASES = [[STABLE]]
REFS = {"2.0.6": BASE}


class NumericReleaseTests(unittest.TestCase):
    def fake_git(self, messages="", refs=None, fail_ancestry=False):
        refs = refs or REFS
        def run(*args):
            if args[:2] == ("git", "rev-parse"):
                ref = args[2].removesuffix("^{commit}")
                return refs.get(ref, ref)
            if args[:2] == ("git", "merge-base"):
                if fail_ancestry:
                    raise release.subprocess.CalledProcessError(1, args)
                return ""
            if args[:2] == ("git", "log"):
                return messages
            raise AssertionError(f"Unexpected command: {args}")
        return run

    def test_each_fix_advances_patch_and_features_advance_minor(self):
        fixes = ["fix(ui): correction"] * 10
        self.assertEqual(release.next_version("2.0.6", fixes), "2.0.16")
        self.assertEqual(release.next_version("2.0.6", ["feat(ui): new option", *fixes]), "2.1.0")
        self.assertEqual(release.next_version("2.0.6", ["feat: one", "feat(core): two", *fixes]), "2.2.0")

    def test_only_conventional_feat_and_fix_headers_count(self):
        self.assertEqual(release.conventional_counts([
            "feat!: breaking header", "fix(scope): correction", "docs: text",
            "feature: not conventional", "Fix: case sensitive",
        ]), (1, 1))
        with self.assertRaisesRegex(ValueError, "No feat or fix"):
            release.next_version("2.0.6", ["docs: update", "chore: tidy"])

    def test_patch_and_windows_numeric_limits_fail_closed(self):
        self.assertEqual(release.next_version("2.0.653", ["fix: patch"]), "2.0.654")
        for base, messages in (("2.0.654", ["fix: overflow"]),
                               ("2.255.0", ["feat: minor overflow"]),
                               ("256.0.0", ["fix: invalid major"])):
            with self.subTest(base=base):
                with self.assertRaisesRegex(ValueError, "Windows installer"):
                    release.next_version(base, messages)

    def test_published_out_of_range_product_version_fails_closed(self):
        releases = [[dict(tag_name="2.0.655", draft=False, prerelease=False,
                          published_at="2026-11-01T00:00:00Z")]]
        with self.assertRaisesRegex(ValueError, "exceeds supported Windows numeric version bounds"):
            release.resolve(releases, {"2.0.655": BASE}, SOURCE, "master", "stable")

    def test_numeric_releases_ignore_legacy_suffix_for_version_math(self):
        releases = [[dict(tag_name="2.0.6-stable.1", draft=False, prerelease=False,
                          published_at="2026-11-01T00:00:00Z")]]
        self.assertEqual(release.latest_stable_release(releases)["tag_name"], "2.0.6-stable.1")
        self.assertEqual(release.next_version("2.0.6-stable.1", ["fix: repair"]), "2.0.7")

    def test_allocator_uses_exact_release_source(self):
        messages = "fix: one\nfix: two\n"
        with patch.object(release, "run", side_effect=self.fake_git(messages, REFS)) as commands:
            result = release.resolve(RELEASES, REFS, SOURCE, "master", "stable")
        self.assertEqual(result["tag"], "2.0.8")
        self.assertEqual(result["channel"], "stable")
        self.assertEqual(result["should_release"], "true")
        self.assertEqual(result["tag_exists"], "false")
        self.assertEqual(commands.call_args_list[0].args, ("git", "rev-parse", f"{BASE}^{{commit}}"))

    def test_nightly_source_baseline_and_highest_published_version_base_are_distinct(self):
        nightly = {"tag_name": "2.0.5-nightly.d20261103.aaaaaaaaaaaa", "draft": False,
                   "prerelease": True, "published_at": "2026-11-03T07:29:00Z"}
        stable = dict(STABLE, published_at="2026-11-03T04:07:00Z")
        releases = [[stable, nightly]]
        nightly_commit = "c" * 40
        refs = {"2.0.6": BASE, nightly["tag_name"]: nightly_commit}
        qualified = {"tag": nightly["tag_name"], "source_commit": nightly_commit,
                     "version": "2.0.5-nightly.d20261103.aaaaaaaaaaaa"}
        with patch.object(release, "run", side_effect=self.fake_git("\n".join(["fix: one"] * 10), refs)):
            result = release.resolve(releases, refs, SOURCE, "canary", "nightly",
                                     {nightly["tag_name"]: "nightly"}, qualified)
        self.assertEqual(result["tag"], "2.0.16")
        self.assertEqual(result["previous_tag"], nightly["tag_name"])

    def test_merge_of_independently_published_sources_counts_only_new_fix(self):
        with tempfile.TemporaryDirectory() as temp:
            def git(*args):
                return subprocess.run(["git", *args], cwd=temp, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "main")
            git("config", "user.name", "Version test")
            git("config", "user.email", "versions@example.invalid")
            git("commit", "--allow-empty", "-m", "chore: root")
            root = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-m", "fix: shipped stable")
            stable_commit = git("rev-parse", "HEAD")
            git("tag", "2.0.6")
            git("checkout", "-b", "nightly", root)
            git("commit", "--allow-empty", "-m", "fix: shipped nightly")
            nightly_commit = git("rev-parse", "HEAD")
            nightly_tag = "2.0.5-nightly.d20261103." + nightly_commit[:12]
            git("tag", nightly_tag)
            git("merge", "main", "-m", "Merge published stable source")
            git("commit", "--allow-empty", "-m", "fix: new fix")
            source = git("rev-parse", "HEAD")

            releases = [[
                dict(tag_name="2.0.6", draft=False, prerelease=False,
                     published_at="2026-11-03T04:07:00Z"),
                dict(tag_name=nightly_tag, draft=False, prerelease=True,
                     published_at="2026-11-03T07:29:00Z"),
            ]]
            refs = {"2.0.6": stable_commit, nightly_tag: nightly_commit}
            qualified = {"tag": nightly_tag, "source_commit": nightly_commit,
                         "version": "2.0.5-nightly.d20261103." + nightly_commit[:12]}
            def scoped_run(*args):
                return subprocess.run(args, cwd=temp, check=True,
                                      capture_output=True, text=True).stdout.strip()
            with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"), \
                 patch.object(release, "run", side_effect=scoped_run):
                result = release.resolve(releases, refs, source, "canary", "nightly",
                                         {nightly_tag: "nightly"}, qualified)
            self.assertEqual(result["version"], "2.0.7")
            self.assertEqual(result["previous_source_commit"], nightly_commit)

    def test_generated_notes_keep_valid_full_sha_for_every_commit(self):
        with tempfile.TemporaryDirectory() as temp:
            def git(*args):
                return subprocess.run(["git", *args], cwd=temp, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "main")
            git("config", "user.name", "Notes test")
            git("config", "user.email", "notes@example.invalid")
            git("commit", "--allow-empty", "-m", "chore: baseline")
            baseline = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-m", "fix: first note", "-m", "body for first note")
            first = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-m", "feat: second note")
            second = git("rev-parse", "HEAD")
            previous_cwd = os.getcwd()
            try:
                os.chdir(temp)
                output = Path(temp) / "notes.md"
                release.generate_notes("2.1.0", second, "stable", [baseline], output)
            finally:
                os.chdir(previous_cwd)
            notes = output.read_text(encoding="utf-8")
            self.assertIn(f"{first[:12]}](https://github.com/Nonary/vibeshine/commit/{first}) fix: first note", notes)
            self.assertIn("body for first note", notes)
            self.assertIn(f"{second[:12]}](https://github.com/Nonary/vibeshine/commit/{second}) feat: second note", notes)
            self.assertNotIn("\n" + second[:12], notes)

    def test_unrelated_imported_date_tags_do_not_claim_product_versions(self):
        refs = {**REFS, "v2025.814.190855": "foreign-a", "v2026.221.41532": "foreign-b"}
        self.assertEqual(set(release.tag_claims(refs)), {(2, 0, 6)})
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            self.assertEqual(release.resolve(RELEASES, refs, SOURCE, "canary", "nightly")["tag"], "2.0.7")

    def test_v_prefixed_claim_on_candidate_is_skipped(self):
        refs = {**REFS, "v2.0.7": "reserved-tag"}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve(RELEASES, refs, SOURCE, "canary", "nightly")
        self.assertEqual(result["tag"], "2.0.8")
        self.assertEqual(result["tag_exists"], "false")

    def test_missing_published_tag_only_widens_the_count(self):
        self.assertEqual(release.published_source_commits(RELEASES, {}), [])
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", {})):
            self.assertEqual(release.resolve(RELEASES, {}, SOURCE, "canary", "nightly")["tag"], "2.0.7")

    def test_latest_publication_timestamp_tie_is_deterministic(self):
        release_a = dict(STABLE, tag_name="2.0.6")
        release_b = dict(STABLE, tag_name="2.0.6-beta.1")
        self.assertEqual(release.latest_release([[release_b, release_a]]),
                         release.latest_release([[release_a, release_b]]))

    def test_unchanged_published_source_skips_without_allocating(self):
        with patch.object(release, "run", return_value=BASE):
            result = release.resolve(RELEASES, REFS, BASE, "master", "prerelease")
        self.assertEqual(result["should_release"], "false")
        self.assertNotIn("version", result)

    def test_existing_tag_is_reused_only_for_same_source_and_channel(self):
        refs = {**REFS, "2.0.7": SOURCE}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve(RELEASES, refs, SOURCE, "master", "prerelease",
                                      {"2.0.7": "prerelease"})
        self.assertEqual(result["tag"], "2.0.7")
        self.assertEqual(result["tag_exists"], "true")
        # Another channel's reservation at this source is a claim, not a retry.
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve(RELEASES, refs, SOURCE, "master", "stable", {"2.0.7": "prerelease"})
        self.assertEqual(result["tag"], "2.0.8")
        self.assertEqual(result["tag_exists"], "false")

    def test_annotated_numeric_tag_is_validated_against_source_and_channel(self):
        refs = {**REFS, "2.0.7": SOURCE}
        with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"), \
             patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.validate_tag(RELEASES, refs, {"2.0.7": "stable"},
                                           "2.0.7", SOURCE, "master", "master",
                                           "Nonary/vibeshine:master", "stable",
                                           source_branch_commit=SOURCE)
        self.assertEqual(result["stable_anchor_tag"], "2.0.6")
        self.assertEqual(result["stable_anchor_commit"], BASE)
        with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"):
            with self.assertRaisesRegex(ValueError, "immutable channel"):
                release.validate_tag(RELEASES, refs, {"2.0.7": "prerelease"},
                                     "2.0.7", SOURCE, "master", "master",
                                     "Nonary/vibeshine:master", "stable")
            with self.assertRaisesRegex(ValueError, "exact release source"):
                with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
                    release.validate_tag(RELEASES, refs, {"2.0.7": "stable"},
                                         "2.0.7", BASE, "master", "master",
                                         "Nonary/vibeshine:master", "stable",
                                         source_branch_commit=SOURCE)

    def test_actual_annotated_tag_channel_and_source_validate_end_to_end(self):
        with tempfile.TemporaryDirectory() as temp:
            def git(*args):
                return subprocess.run(["git", *args], cwd=temp, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "master")
            git("config", "user.name", "Version test")
            git("config", "user.email", "versions@example.invalid")
            git("commit", "--allow-empty", "-m", "chore: root")
            git("commit", "--allow-empty", "-m", "fix: baseline")
            stable_commit = git("rev-parse", "HEAD")
            git("tag", "2.0.6")
            git("commit", "--allow-empty", "-m", "fix: allocated source")
            source = git("rev-parse", "HEAD")
            git("tag", "-a", "2.0.7", "-m", "Vibeshine release channel: stable\nSource branch: master")
            scoped_run = lambda *args: subprocess.run(
                args, cwd=temp, check=True, capture_output=True, text=True).stdout.strip()
            with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"), \
                 patch.object(release, "run", side_effect=scoped_run):
                refs, channels = release.read_refs()
                result = release.validate_tag(
                    RELEASES, refs, channels, "2.0.7", source, "master", "master",
                    "Nonary/vibeshine:master", "stable", source_branch_commit=source)
            self.assertEqual(channels["2.0.7"], "stable")
            self.assertEqual(result["tag"], "2.0.7")
            with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"), \
                 patch.object(release, "run", side_effect=scoped_run):
                with self.assertRaisesRegex(ValueError, "not reachable from the confirmed source branch"):
                    release.validate_tag(RELEASES, refs, channels, "2.0.7", source,
                                         "master", "master", "Nonary/vibeshine:master",
                                         "stable", source_branch_commit=stable_commit)
            with patch.dict("os.environ", GITHUB_REPOSITORY="Nonary/vibeshine"), \
                 patch.object(release, "run", side_effect=scoped_run):
                with self.assertRaisesRegex(ValueError, "immutable channel"):
                    release.validate_tag(RELEASES, refs, channels, "2.0.7", source,
                                         "master", "master", "Nonary/vibeshine:master",
                                         "prerelease")

    def test_orphaned_reservation_from_failed_build_is_skipped(self):
        refs = {**REFS, "2.0.7": "other-tag-object"}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve(RELEASES, refs, SOURCE, "canary", "nightly", {"2.0.7": "nightly"})
        self.assertEqual(result["tag"], "2.0.8")
        self.assertEqual(result["tag_exists"], "false")

    def test_source_without_latest_stable_still_numbers_above_it(self):
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", REFS, fail_ancestry=True)):
            result = release.resolve(RELEASES, REFS, SOURCE, "canary", "nightly")
        self.assertEqual(result["tag"], "2.0.7")
        self.assertEqual(result["should_release"], "true")

    def test_suffixed_tag_on_candidate_core_is_skipped(self):
        refs = {**REFS, "2.0.7-stable.1": "legacy-stable-ref"}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            self.assertEqual(release.resolve(RELEASES, refs, SOURCE, "master", "stable")["tag"], "2.0.8")

    def test_skip_after_last_patch_overflows_fails_closed(self):
        releases = [[dict(STABLE, tag_name="2.0.653")]]
        refs = {"2.0.653": BASE, "2.0.654": "other-tag-object"}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            with self.assertRaisesRegex(ValueError, "Windows installer"):
                release.resolve(releases, refs, SOURCE, "canary", "nightly")

    def test_published_nightly_raises_the_base_above_stable(self):
        nightly = dict(tag_name="2.1.0", draft=False, prerelease=True,
                       published_at="2026-11-07T07:00:00Z")
        refs = {**REFS, "2.1.0": "c" * 40}
        qualified = {"tag": "2.1.0", "version": "2.1.0", "source_commit": "c" * 40}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\nfix: two\n", refs)):
            result = release.resolve([[STABLE, nightly]], refs, SOURCE, "canary", "nightly",
                                     {"2.1.0": "nightly"}, qualified)
        self.assertEqual(result["tag"], "2.1.2")
        self.assertEqual(result["previous_tag"], "2.1.0")

    def test_stable_published_above_nightly_raises_the_next_nightly(self):
        nightly = dict(tag_name="2.1.3", draft=False, prerelease=True,
                       published_at="2026-11-07T07:00:00Z")
        stable = dict(STABLE, tag_name="2.2.0", published_at="2026-11-08T07:00:00Z")
        refs = {"2.1.3": "c" * 40, "2.2.0": BASE}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve([[stable, nightly]], refs, SOURCE, "canary", "nightly")
        self.assertEqual(result["tag"], "2.2.1")

    def test_retry_reservation_is_dropped_once_a_newer_release_is_published(self):
        newer = dict(STABLE, tag_name="2.0.8", published_at="2026-11-02T00:00:00Z")
        refs = {**REFS, "2.0.7": SOURCE, "2.0.8": BASE}
        with patch.object(release, "run", side_effect=self.fake_git("fix: one\n", refs)):
            result = release.resolve([[STABLE, newer]], refs, SOURCE, "canary", "nightly",
                                     {"2.0.7": "nightly"})
        self.assertEqual(result["tag"], "2.0.9")
        self.assertEqual(result["tag_exists"], "false")

    def test_no_feat_or_fix_since_published_sources_skips(self):
        with patch.object(release, "run", side_effect=self.fake_git("docs: notes\nci: tidy\n", REFS)):
            result = release.resolve(RELEASES, REFS, SOURCE, "canary", "nightly")
        self.assertEqual(result["should_release"], "false")
        self.assertIn("No feat or fix", result["skip_reason"])

    def test_canary_missing_published_stable_numbers_above_it_end_to_end(self):
        """Regression: canary without stable 2.0.6 must never produce 2.0.5 or 2.0.6."""
        with tempfile.TemporaryDirectory() as temp:
            def git(*args):
                return subprocess.run(["git", *args], cwd=temp, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "master")
            git("config", "user.name", "Version test")
            git("config", "user.email", "versions@example.invalid")
            git("commit", "--allow-empty", "-m", "chore: root")
            git("tag", "2.0.4")
            root = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-m", "fix: stable only")
            git("tag", "2.0.6")
            stable_commit = git("rev-parse", "HEAD")
            git("checkout", "-q", "-b", "canary", root)
            git("commit", "--allow-empty", "-m", "fix: shipped nightly")
            nightly_commit = git("rev-parse", "HEAD")
            nightly_tag = "2.0.5-nightly.d20261106." + nightly_commit[:12]
            git("tag", nightly_tag)
            git("commit", "--allow-empty", "-m", "fix(audio): new fix")
            git("commit", "--allow-empty", "-m", "fix(gamepad): another fix")
            fixes_only = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-m", "feat(stream): new feature")
            git("commit", "--allow-empty", "-m", "fix(stream): follow-up")
            with_feature = git("rev-parse", "HEAD")

            releases = [[
                dict(tag_name="2.0.4", draft=False, prerelease=False, published_at="2026-11-01T23:51:16Z"),
                dict(tag_name="2.0.6", draft=False, prerelease=False, published_at="2026-11-03T04:07:29Z"),
                dict(tag_name=nightly_tag, draft=False, prerelease=True, published_at="2026-11-06T07:26:35Z"),
            ]]
            qualified = {"tag": nightly_tag, "source_commit": nightly_commit, "version": nightly_tag}
            scoped_run = lambda *args: subprocess.run(
                args, cwd=temp, check=True, capture_output=True, text=True).stdout.strip()
            with patch.object(release, "run", side_effect=scoped_run):
                refs, channels = release.read_refs()
                fixes = release.resolve(releases, refs, fixes_only, "canary", "nightly", channels, qualified)
                feature = release.resolve(releases, refs, with_feature, "canary", "nightly", channels, qualified)
            self.assertEqual(fixes["tag"], "2.0.8")
            self.assertEqual(feature["tag"], "2.1.0")
            self.assertEqual(fixes["previous_source_commit"], nightly_commit)
            self.assertNotEqual(stable_commit, nightly_commit)


if __name__ == "__main__":
    unittest.main()
