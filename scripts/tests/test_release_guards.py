import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from contextlib import contextmanager
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys_path = str(ROOT)
import sys
if sys_path not in sys.path:
    sys.path.insert(0, sys_path)
spec = importlib.util.spec_from_file_location("daily_release", ROOT / "daily_release.py")
daily = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daily)


class ReleaseGuardTests(unittest.TestCase):
    @contextmanager
    def synthetic_history(self):
        with tempfile.TemporaryDirectory() as temp:
            repo = Path(temp)
            def git(*args):
                return subprocess.run(["git", *args], cwd=repo, check=True,
                                      capture_output=True, text=True).stdout.strip()
            git("init", "-q", "-b", "master")
            git("config", "user.name", "Release Test")
            git("config", "user.email", "release@example.invalid")
            def commit(message):
                (repo / "history.txt").write_text(message + "\n", encoding="utf-8")
                git("add", "history.txt")
                git("commit", "-qm", message)
                return git("rev-parse", "HEAD")
            root = commit("chore: test base")
            stable = commit("fix: stable source")
            git("tag", "2.0.6", stable)
            git("checkout", "-qb", "nightly", root)
            legacy = commit("fix: legacy nightly source")
            legacy_tag = f"2.0.2-nightly.d20261103.{legacy[:12]}"
            git("tag", legacy_tag, legacy)
            numeric = commit("fix: numeric nightly source")
            git("tag", "2.0.7", numeric)
            metadata = {
                11: {"schema_version": 1, "channel": "nightly", "tag": legacy_tag,
                     "version": legacy_tag, "source_commit": legacy,
                     "nightly_sequence": 20261103},
                12: {"schema_version": 1, "channel": "nightly", "tag": "2.0.7",
                     "version": "2.0.7", "source_commit": numeric},
            }
            releases = [[
                {"tag_name": "2.0.6", "draft": False, "prerelease": False,
                 "published_at": "2026-11-01T00:00:00Z", "id": 1, "assets": []},
                {"tag_name": legacy_tag, "draft": False, "prerelease": True,
                 "published_at": "2026-11-02T00:00:00Z", "id": 2,
                 "assets": [{"name": "nightly-metadata.json", "id": 11}]},
                {"tag_name": "2.0.7", "draft": False, "prerelease": True,
                 "published_at": "2026-11-03T00:00:00Z", "id": 3,
                 "assets": [{"name": "nightly-metadata.json", "id": 12}]},
            ]]
            yield repo, git, root, stable, legacy, numeric, legacy_tag, releases, metadata

    @contextmanager
    def in_directory(self, path):
        original = os.getcwd()
        os.chdir(path)
        try:
            yield
        finally:
            os.chdir(original)

    def real_history_run(self, repo, releases, metadata, target):
        def run(*args):
            if args[0] == "gh" and args[1:3] == ("api", "--paginate"):
                return json.dumps(releases)
            if args[0] == "gh" and "releases/assets/" in args[-1]:
                return json.dumps(metadata[int(args[-1].split("/")[-1])])
            if args[:2] == ("git", "fetch"):
                return ""
            if args[:2] == ("git", "rev-parse") and "refs/remotes/origin/daily-source" in args[-1]:
                return target
            return subprocess.run(args, check=True, capture_output=True, text=True).stdout.strip()
        return run

    def test_nightly_publication_rechecks_exact_stable_anchor_pair(self):
        source = "a" * 40
        old_stable_source = "b" * 40
        retargeted_source = "c" * 40

        def run(*args):
            if args[:2] == ("git", "rev-parse"):
                return source if args[-1] == "refs/tags/2.0.7^{commit}" else old_stable_source
            if args[:2] == ("gh", "api"):
                return "[]"
            if args[:2] == ("git", "tag"):
                return ""
            raise AssertionError(args)

        with patch.object(daily, "run", side_effect=run), \
             patch.object(daily.numeric_release, "verify_annotated_reservation"), \
             patch.object(daily, "verify_remote_numeric_reservation"), \
             patch.object(daily, "published_release_baseline", return_value=((2, 0, 6), "2.0.6", old_stable_source)), \
             patch.object(daily, "latest_published_release", return_value=("2.0.6", {})), \
             patch.object(daily, "published_stable_anchor", return_value=((2, 0, 6), "2.0.6")), \
             patch.object(daily, "published_source_commits", return_value=[old_stable_source]), \
             patch.object(daily.numeric_release, "commit_messages_since_sources", return_value=["fix: test"]), \
             patch.object(daily.numeric_release, "allocate_version", return_value="2.0.7"), \
             patch.object(daily, "published_release_source", return_value=None), \
             patch.object(daily, "previous_successful_nightly", return_value=None), \
             patch.object(daily.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(ValueError, "Stable anchor changed"):
                daily.guard_publication("Nonary/Vibepollo", source, "2.0.7",
                                        expected_stable_tag="2.0.6",
                                        expected_stable_commit=retargeted_source)

    def test_release_selectors_validate_mixed_stable_legacy_and_numeric_history(self):
        with self.synthetic_history() as (repo, git, root, stable, legacy, numeric,
                                          legacy_tag, releases, metadata):
            with self.in_directory(repo), patch.object(
                    daily, "run", side_effect=self.real_history_run(repo, releases, metadata, numeric)):
                base, base_tag, base_source = daily.published_release_baseline("Nonary/Vibepollo", releases)
                latest_tag, _ = daily.latest_published_release(releases)
                sources = daily.published_source_commits("Nonary/Vibepollo", releases)
            self.assertEqual((base, base_tag, base_source), ((2, 0, 7), "2.0.7", numeric))
            self.assertEqual(latest_tag, "2.0.7")
            self.assertEqual(sources, [stable, legacy, numeric])

            wrong = [[dict(releases[0][2], assets=[{"name": "nightly-metadata.json", "id": 12}])]]
            bad_metadata = dict(metadata)
            bad_metadata[12] = dict(metadata[12], source_commit=legacy)
            with self.in_directory(repo), patch.object(
                    daily, "run", side_effect=self.real_history_run(repo, wrong, bad_metadata, numeric)):
                with self.assertRaisesRegex(ValueError, "does not match its immutable tag"):
                    daily.published_source_commits("Nonary/Vibepollo", wrong)

    def test_resolve_skips_an_exact_source_already_released(self):
        with self.synthetic_history() as (repo, git, root, stable, legacy, numeric,
                                          legacy_tag, releases, metadata):
            with tempfile.TemporaryDirectory() as temp, self.in_directory(repo):
                output = Path(temp) / "output"
                with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=numeric,
                                DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                                DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master",
                                GITHUB_EVENT_NAME="schedule", GITHUB_REF_TYPE="branch",
                                GITHUB_REF_NAME="master", GITHUB_OUTPUT=str(output)):
                    with patch.object(daily, "run", side_effect=self.real_history_run(repo, releases, metadata, numeric)):
                        daily.resolve()
                self.assertIn("should_build=false", output.read_text())
                self.assertIn("Source already published as 2.0.7", output.read_text())

    def test_disjoint_stable_and_nightly_anchors_fail_closed(self):
        with self.synthetic_history() as (repo, git, root, stable, legacy, numeric,
                                          legacy_tag, releases, metadata):
            git("checkout", "-qb", "candidate-stable", stable)
            git("commit", "--allow-empty", "-m", "fix: stable-only candidate")
            stable_only = git("rev-parse", "HEAD")
            git("checkout", "-qb", "candidate-nightly", legacy)
            git("commit", "--allow-empty", "-m", "fix: nightly-only candidate")
            nightly_only = git("rev-parse", "HEAD")
            releases = [[releases[0][0], releases[0][1]]]
            metadata = {11: metadata[11]}
            for target, missing_anchor in ((stable_only, legacy), (nightly_only, stable)):
                with self.subTest(target=target), tempfile.TemporaryDirectory() as temp, self.in_directory(repo):
                    output = Path(temp) / "output"
                    with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=target,
                                    DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                                    DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master",
                                    GITHUB_EVENT_NAME="schedule", GITHUB_REF_TYPE="branch",
                                    GITHUB_REF_NAME="master", GITHUB_OUTPUT=str(output)):
                        with patch.object(daily, "run", side_effect=self.real_history_run(repo, releases, metadata, target)):
                            with self.assertRaises(subprocess.CalledProcessError) as error:
                                daily.resolve()
                    self.assertIn(missing_anchor, error.exception.cmd)
                    self.assertFalse(output.exists())

    def test_annotated_numeric_reservation_retry_is_channel_bound(self):
        with self.synthetic_history() as (repo, git, root, stable, legacy, source,
                                          legacy_tag, releases, metadata), self.in_directory(repo):
            def remote_refs(*args):
                if args[:3] == ("git", "ls-remote", "--tags"):
                    refs = []
                    for ref in args[4:]:
                        try:
                            obj = git("rev-parse", ref)
                        except subprocess.CalledProcessError:
                            continue
                        refs.append(f"{obj} {ref}")
                        try:
                            peeled = git("rev-parse", ref + "^{}")
                            if peeled != obj:
                                refs.append(f"{peeled} {ref}^{{}}")
                        except subprocess.CalledProcessError:
                            pass
                    return "\n".join(refs)
                if args[:2] == ("git", "fetch"):
                    return ""
                raise AssertionError(args)

            version = "2.0.8"
            message = f"Vibepollo release channel: nightly\nsource: {source}"
            git("tag", "-a", "-m", message, version, source)
            nightly_draft = {"tag_name": version, "draft": True, "prerelease": True}
            with patch.object(daily, "run", side_effect=remote_refs):
                found = daily.reserve_numeric_identity(source, version, [[nightly_draft]], "nightly")
            self.assertEqual(found, {"commit": source, "tag": version, "version": version, "channel": "nightly"})

            git("tag", "-f", "-a", "-m", f"Vibepollo release channel: stable\nsource: {source}", version, source)
            with patch.object(daily, "run", side_effect=remote_refs):
                with self.assertRaisesRegex(ValueError, "channel and source"):
                    daily.reserve_numeric_identity(source, version, [], "nightly")

            git("tag", "v2.0.9", source)
            with patch.object(daily, "run", side_effect=remote_refs):
                with self.assertRaisesRegex(ValueError, "v-prefixed"):
                    daily.reserve_numeric_identity(source, "2.0.9", [], "nightly")

            git("tag", "-a", "-m", f"Vibepollo release channel: nightly\nsource: {stable}",
                "2.0.10", stable)
            with patch.object(daily, "run", side_effect=remote_refs):
                with self.assertRaisesRegex(ValueError, "another source"):
                    daily.reserve_numeric_identity(source, "2.0.10", [], "nightly")

    def test_live_remote_tag_retarget_and_channel_changes_fail(self):
        tag, source, object_sha = "2.0.7", "a" * 40, "c" * 40
        ref = {"ref": "refs/tags/2.0.7", "object": {"type": "tag", "sha": object_sha}}
        tag_object = {"tag": tag, "object": {"type": "commit", "sha": source},
                      "message": f"Vibepollo release channel: nightly\nsource: {source}"}
        with patch.object(daily, "run", side_effect=[json.dumps(ref), json.dumps(tag_object)]):
            daily.verify_remote_numeric_reservation("Nonary/Vibepollo", tag, source, "nightly")
        for field, value in (("object", {"type": "commit", "sha": "b" * 40}),
                             ("message", f"Vibepollo release channel: stable\nsource: {source}")):
            changed = dict(tag_object)
            changed[field] = value
            with self.subTest(field=field), patch.object(daily, "run", side_effect=[json.dumps(ref), json.dumps(changed)]):
                with self.assertRaisesRegex(ValueError, "changed version, channel or exact source"):
                    daily.verify_remote_numeric_reservation("Nonary/Vibepollo", tag, source, "nightly")

    def test_signing_auto_discovery_accepts_only_validated_ci_triggers(self):
        workflow = (ROOT.parent / ".github/workflows/sign-release.yml").read_text()
        self.assertIn('and (.event == "push" or .event == "workflow_dispatch")', workflow)
        self.assertIn('("${source_event}" != "push" && "${source_event}" != "workflow_dispatch")', workflow)
        self.assertIn('"${source_sha}" != "${release_commit}"', workflow)
        self.assertIn('"${source_branch}" != "${source_tag}"', workflow)

    def test_numeric_signing_release_is_created_hidden_then_verified_before_publish(self):
        workflow = (ROOT.parent / ".github/workflows/sign-release.yml").read_text()
        self.assertIn('Numeric release ${TAG_NAME} exists with a different state or identity', workflow)
        self.assertIn('create_draft=true', workflow)
        self.assertIn('if [[ "${numeric_identity}" != "true" ]]; then', workflow)
        self.assertIn('if [[ "${actual_names}" != "${expected_names}" ]]', workflow)
        self.assertIn('asset_digest', workflow)
        self.assertIn('Existing numeric release asset differs from this exact build', workflow)
        self.assertIn('Existing numeric draft notes do not match this exact source/channel candidate', workflow)
        self.assertIn("jq -j '.body // \"\"'", workflow)
        self.assertIn('Nightly draft notes no longer match the exact source/channel candidate', (ROOT / "daily_release.py").read_text())
        self.assertIn('Stable draft notes or identity changed before publication', (ROOT / "daily_release.py").read_text())
        self.assertIn('final_payload=', workflow)
        self.assertLess(workflow.index('local_digest='), workflow.index('final_payload='))

    def test_signing_cannot_create_new_suffixed_release_identities(self):
        workflow = (ROOT.parent / ".github/workflows/sign-release.yml").read_text()
        self.assertIn('New suffixed Vibepollo release identities are disabled', workflow)
        self.assertIn('existing_legacy_releases', workflow)

    def test_daily_validation_only_skips_remote_tag_reservation(self):
        workflow = (ROOT.parent / ".github/workflows/daily-prerelease.yml").read_text()
        reservation_step = workflow.split("- name: Reserve exact qualified source identity before builds", 1)[1].split("  build:", 1)[0]
        self.assertIn("vars.DAILY_RELEASE_ENABLED == 'true' && inputs.publish == true", reservation_step)
        self.assertIn("daily_validation_only: ${{ vars.DAILY_RELEASE_ENABLED != 'true' || inputs.publish != true }}", workflow)
        self.assertIn("release_channel: nightly", reservation_step + workflow.split("  build:", 1)[1])
        self.assertIn('Existing nightly draft notes do not match this exact source/channel candidate', workflow)
        self.assertIn("jq -j '.body // \"\"'", workflow)


if __name__ == "__main__":
    unittest.main()
