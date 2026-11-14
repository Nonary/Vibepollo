import datetime as dt
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("daily", Path(__file__).resolve().parents[1] / "daily_release.py")
daily = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daily)
SHA = "a" * 40
BASE = "c" * 40
STABLE = dict(tag_name="2.0.0", draft=False, prerelease=False, published_at="2026-10-01T00:00:00Z")
STABLE_PAGES = [[STABLE]]


class DailyTests(unittest.TestCase):
    def setUp(self):
        env = patch.dict(os.environ, GITHUB_EVENT_NAME="workflow_dispatch", GITHUB_REF_TYPE="branch", GITHUB_REF_NAME="master")
        env.start()
        self.addCleanup(env.stop)

    def test_stable_anchor_uses_only_published_stable_releases(self):
        releases = [[STABLE,
                     dict(tag_name="2.5.0", draft=True, prerelease=False, published_at="today"),
                     dict(tag_name="9.0.0", draft=False, prerelease=True, published_at="today")],
                    [dict(tag_name="v2.0.6", draft=False, prerelease=False, published_at="today"),
                     dict(tag_name="2.0.7", draft=False, prerelease=False, published_at=None)]]
        self.assertEqual(daily.published_stable_anchor(releases), ((2, 0, 6), "v2.0.6"))
        with self.assertRaisesRegex(ValueError, "No published stable"):
            daily.published_stable_anchor([[dict(tag_name="2.0.7", draft=True, prerelease=False, published_at="today")]])

    def test_nightly_numbers_above_published_prerelease_without_any_stable(self):
        releases = [[dict(tag_name="2.0.7-beta.1", draft=False, prerelease=True, published_at="today")]]
        with patch.object(daily.numeric_release, "run",
                          side_effect=lambda *args: "fix: one" if args[1] == "log" else BASE):
            result = daily.numeric_release.resolve(releases, {"2.0.7-beta.1": BASE}, SHA,
                                                   "canary", "nightly")
        self.assertEqual(result["tag"], "2.0.8")

    def test_stable_anchor_is_informational_and_never_blocks(self):
        self.assertEqual(daily.stable_anchor([[]]), ("", ""))
        with patch.object(daily, "run", return_value=BASE):
            self.assertEqual(daily.stable_anchor(STABLE_PAGES), ("2.0.0", BASE))

    def test_numeric_publication_requires_the_reserved_tag_to_still_be_current(self):
        stable_release = dict(tag_name="2.0.6", draft=False, prerelease=False,
                              published_at="2026-11-01T00:00:00Z")
        pages = [[stable_release]]
        refs = {"2.0.6": BASE}
        with patch.dict(os.environ, DAILY_RELEASE_SOURCE_BRANCH="canary"):
            with patch.object(daily, "run", return_value=json.dumps(pages)), \
                 patch.object(daily, "tag_refs", return_value=(refs, {})), \
                 patch.object(daily, "previous_successful_nightly", return_value=None), \
                 patch.object(daily.numeric_release, "resolve", return_value={
                     "should_release": "true", "tag": "2.0.7"}):
                daily.validate_live_numeric_version("Nonary/vibeshine", "2.0.7", SHA, "nightly")
                with self.assertRaisesRegex(ValueError, "Published release state changed"):
                    daily.validate_live_numeric_version("Nonary/vibeshine", "2.0.8", SHA, "nightly")

    def test_linux_requires_exact_source_version_run_and_hash(self):
        for scenario in ('valid', 'source', 'version', 'run', 'hash', 'extra'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temp:
                root, linux = Path(temp) / 'assets', Path(temp) / 'linux'
                root.mkdir(); linux.mkdir()
                name = 'vibeshine-2.0.0.daily-1-x86_64.pkg.tar.zst'
                (linux / name).write_bytes(b'package')
                evidence = dict(source_commit=SHA, version='version', workflow_run_id='123', assets={name: hashlib.sha256(b'package').hexdigest()})
                if scenario == 'source': evidence['source_commit'] = 'b' * 40
                if scenario == 'version': evidence['version'] = 'other'
                if scenario == 'run': evidence['workflow_run_id'] = '456'
                if scenario == 'hash': (linux / name).write_bytes(b'tampered')
                if scenario == 'extra': (linux / 'vibeshine-extra.pkg.tar.zst').write_bytes(b'other')
                (linux / 'linux-provenance.json').write_text(json.dumps(evidence))
                (root / 'daily-manifest.json').write_text(json.dumps({'assets': {}}))
                with patch.dict(os.environ, GITHUB_RUN_ID='123'):
                    if scenario == 'valid':
                        daily.include_linux(root, linux, 'Vibeshine', SHA, 'version')
                        self.assertEqual((root / name).read_bytes(), b'package')
                        self.assertEqual(json.loads((root / 'daily-manifest.json').read_text())['linux'], evidence)
                    else:
                        with self.assertRaises(ValueError): daily.include_linux(root, linux, 'Vibeshine', SHA, 'version')

    def test_calendar_identity_fits_msi(self):
        first = daily.identity(SHA, dt.date(2026, 10, 11), "2.0.0")
        self.assertEqual(first["tag"], "2.0.0-nightly.d20261011.aaaaaaaaaaaa")
        self.assertEqual(first["version"], first["tag"])
        for day in (dt.date(2026, 12, 31), dt.date(2027, 1, 1), dt.date(2028, 2, 29)):
            major, minor, patch_number = daily.identity(SHA, day, "2.0.0")["version"].split("-")[0].split(".")
            self.assertLessEqual(int(major), 255)
            self.assertLessEqual(int(minor), 255)
            self.assertLessEqual(int(patch_number) * 100 + 90, 65534)
        with self.assertRaises(ValueError):
            daily.identity("HEAD", dt.date(2026, 10, 11), "2.0.0")
        for base in ("0.0.0", "256.0.0", "2.256.0", "2.0.655", "2.0", "2.0.0-beta.1"):
            with self.assertRaises(ValueError):
                daily.identity(SHA, dt.date(2026, 10, 11), base)

    def test_source_requires_exact_upstream_confirmation(self):
        for repository, branch, confirmation in (("Other/fork", "main", "Other/fork:main"), ("Nonary/vibeshine", "", ""), ("Nonary/vibeshine", "master", "true")):
            with self.assertRaises(ValueError):
                daily.validate_source(repository, branch, confirmation)
        daily.validate_source("Nonary/vibeshine", "master", "Nonary/vibeshine:master")
        daily.validate_source("Nonary/vibeshine", "canary", "Nonary/vibeshine:canary")
        for repository, confirmation in (("Nonary/vibeshine", "Nonary/vibeshine:master"),
                                         ("Nonary/Vibepollo", "Nonary/Vibepollo:master")):
            with self.assertRaises(ValueError):
                daily.validate_source(repository, "canary", confirmation)
        for retired in ("vibe", "vibe-test"):
            with self.assertRaises(ValueError):
                daily.validate_source("Nonary/vibeshine", retired, f"Nonary/vibeshine:{retired}")

    def test_reserved_numeric_retry_is_covered_by_shared_allocator_contract(self):
        self.assertIn("tag_exists", Path(__file__).resolve().parents[1].joinpath("numeric_release.py").read_text())

    def test_published_same_day_nightly_skips_even_when_source_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(STABLE_PAGES), BASE]):
                    with patch.object(daily, "previous_successful_nightly", return_value={"tag": "2.0.6", "source_commit": SHA}):
                        daily.resolve()
            self.assertIn("should_build=false", output.read_text())
            self.assertIn("Source unchanged since 2.0.6", output.read_text())

    def test_resolve_fetches_confirmed_remote_commit_and_emits_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(STABLE_PAGES), "", BASE]) as commands:
                    with patch.object(daily, "previous_successful_nightly", return_value=None), \
                         patch.object(daily, "tag_refs", return_value=({"2.0.0": BASE}, {})), \
                         patch.object(daily.numeric_release, "resolve", return_value={"commit": SHA, "tag": "2.0.7", "version": "2.0.7", "channel": "nightly", "should_release": "true"}) as allocator:
                        daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("version=2.0.7", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, ("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/daily-source"))
            self.assertEqual(allocator.call_args.args[4], "nightly")

    def test_canary_pins_remote_tip_separately_from_default_workflow_commit(self):
        control = "b" * 40
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=control,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:canary", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(STABLE_PAGES), "", BASE]) as commands:
                    with patch.object(daily, "previous_successful_nightly", return_value=None), \
                         patch.object(daily, "tag_refs", return_value=({"2.0.0": BASE}, {})), \
                         patch.object(daily.numeric_release, "resolve", return_value={"commit": SHA, "tag": "2.0.7", "version": "2.0.7", "channel": "nightly", "should_release": "true"}):
                        daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, (
                "git", "fetch", "--no-tags", "origin", "refs/heads/canary:refs/remotes/origin/daily-source"))
            self.assertIn((("git", "diff", "--exit-code", control, SHA, "--", ".github/workflows"),),
                          commands.call_args_list)

    def test_canary_workflow_drift_blocks_publication(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA="b" * 40,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="canary",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:canary", GITHUB_OUTPUT=str(output)):
                mismatch = daily.subprocess.CalledProcessError(1, ["git", "diff"])
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", mismatch]):
                    with self.assertRaises(daily.subprocess.CalledProcessError):
                        daily.resolve()
            self.assertFalse(output.exists())

    def test_unchanged_repo_skips_before_date_and_upstream_migration_checks(self):
        for repository in ("Nonary/vibeshine", "Nonary/Vibepollo"):
            with self.subTest(repository=repository), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with patch.dict(os.environ, GITHUB_REPOSITORY=repository, GITHUB_SHA=SHA,
                                DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                                DAILY_RELEASE_SOURCE_CONFIRMED=f"{repository}:master", GITHUB_OUTPUT=str(output),
                                DAILY_RELEASE_MIGRATION_CONFIRMED="stale upstream pin"):
                    with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(STABLE_PAGES), BASE]) as commands:
                        with patch.object(daily, "previous_successful_nightly", return_value={"tag": "daily-2026-10-10", "source_commit": SHA}) as baseline:
                            daily.resolve()
                baseline.assert_called_once_with(repository, STABLE_PAGES)
                self.assertIn("should_build=false", output.read_text())
                self.assertNotIn("version=", output.read_text())
                self.assertFalse(any("ls-remote" in call.args for call in commands.call_args_list))

    def test_changed_downstream_source_builds_without_upstream_attestation(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(STABLE_PAGES), "", BASE]) as commands:
                    with patch.object(daily, "previous_successful_nightly", return_value=None), \
                         patch.object(daily, "tag_refs", return_value=({"2.0.0": BASE}, {})), \
                         patch.object(daily.numeric_release, "resolve", return_value={"commit": SHA, "tag": "2.0.7", "version": "2.0.7", "channel": "nightly", "should_release": "true"}) as allocator:
                        daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("version=2.0.7", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, ("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/daily-source"))
            self.assertEqual(allocator.call_args.args[4], "nightly")

    def test_only_published_validated_nightly_is_a_skip_baseline(self):
        releases = [[
            {"tag_name": "daily-2026-10-11", "draft": True, "prerelease": True, "published_at": "2026-10-11T00:00:00Z"},
            {"tag_name": "daily-2026-10-10", "draft": False, "prerelease": True, "published_at": "2026-10-10T00:00:00Z", "assets": [{"name": "nightly-metadata.json", "id": 1}]},
        ]]
        metadata = {"schema_version": 1, "channel": "nightly", "tag": "daily-2026-10-10", "source_commit": SHA,
                    "version": f"2.0.0-daily.d20261010+{SHA[:12]}", "nightly_sequence": 20261010}
        with patch.object(daily, "run", side_effect=[json.dumps(metadata), SHA]):
            self.assertEqual(daily.previous_successful_nightly("Nonary/vibeshine", releases)["source_commit"], SHA)
        with patch.object(daily, "run", side_effect=[json.dumps(metadata), "b" * 40]):
            with self.assertRaisesRegex(ValueError, "tag"):
                daily.previous_successful_nightly("Nonary/vibeshine", releases)
        with patch.object(daily, "run") as commands:
            self.assertIsNone(daily.previous_successful_nightly("Nonary/vibeshine", [[releases[0][0]]]))
            self.assertIsNone(daily.previous_successful_nightly("Nonary/vibeshine", [[
                {"tag_name": "daily-2026-10-09", "draft": False, "prerelease": True, "published_at": "2026-10-09T00:00:00Z", "assets": []}
            ]]))
            commands.assert_not_called()

    def test_finalized_assets_reject_corruption_wrong_run_and_extra_files(self):
        info = daily.identity(SHA, dt.date(2026, 10, 11), "2.0.0")
        for scenario in ("valid", "corrupt", "wrong-run", "wrong-source", "extra", "missing", "public-msi", "unverified-msi"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "VibeshineSetup.exe").write_bytes(b"setup")
                provenance = {"schema_version": 1, "release_tag": info["tag"], "release_commit": SHA, "source_build_run_id": "123", "assets": {"VibeshineSetup.exe": hashlib.sha256(b"setup").hexdigest()}}
                provenance["embedded_msi"] = {"verified": True, "sha256": hashlib.sha256(b"msi").hexdigest()}
                if scenario == "corrupt":
                    (root / "VibeshineSetup.exe").write_bytes(b"tampered")
                if scenario == "wrong-run":
                    provenance["source_build_run_id"] = "456"
                if scenario == "wrong-source":
                    provenance["release_commit"] = "b" * 40
                if scenario == "extra":
                    (root / "local-driver.cat").write_bytes(b"untrusted")
                if scenario == "missing":
                    (root / "VibeshineSetup.exe").unlink()
                if scenario == "public-msi":
                    (root / "Vibeshine.msi").write_bytes(b"msi")
                if scenario == "unverified-msi":
                    provenance["embedded_msi"]["verified"] = False
                (root / "release-provenance.json").write_text(json.dumps(provenance))
                with patch.dict(os.environ, GITHUB_RUN_ID="123"):
                    if scenario == "valid":
                        daily.verify_assets(root, "Vibeshine", SHA, info["tag"], info["version"])
                        manifest = json.loads((root / "daily-manifest.json").read_text())
                        self.assertTrue(manifest["prerelease"])
                        self.assertFalse(manifest["signpath"])
                    else:
                        with self.assertRaises(ValueError):
                            daily.verify_assets(root, "Vibeshine", SHA, info["tag"], info["version"])

    def test_manual_canary_source_is_exact_dispatch_sha(self):
        for advanced in (False, True):
            with self.subTest(advanced=advanced), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / 'output'
                with patch.dict(os.environ, GITHUB_REPOSITORY='Nonary/Vibepollo', GITHUB_SHA=SHA,
                                GITHUB_REF_NAME='canary', DAILY_PRIMARY_BRANCH='master',
                                DAILY_RELEASE_SOURCE_BRANCH='canary',
                                DAILY_RELEASE_SOURCE_CONFIRMED='Nonary/Vibepollo:canary', GITHUB_OUTPUT=str(output)):
                    commands = ['', 'b'*40] if advanced else ['', SHA, '', json.dumps(STABLE_PAGES), '', BASE]
                    with patch.object(daily, 'run', side_effect=commands), \
                         patch.object(daily, 'previous_successful_nightly', return_value=None), \
                         patch.object(daily, 'tag_refs', return_value=({'2.0.0': BASE}, {})), \
                         patch.object(daily.numeric_release, 'resolve', return_value={'commit': SHA, 'tag': '2.1.0', 'version': '2.1.0', 'channel': 'nightly', 'should_release': 'true'}):
                        if advanced:
                            with self.assertRaisesRegex(ValueError, 'advanced after dispatch'): daily.resolve()
                            self.assertFalse(output.exists())
                        else:
                            daily.resolve()
                            self.assertIn('should_build=true', output.read_text())

    def test_invalid_invocations_are_rejected(self):
        for event, ref, kind in [('push', 'canary', 'branch'), ('workflow_dispatch', 'untrusted', 'branch'), ('workflow_dispatch', 'canary', 'tag')]:
            with self.subTest(event=event, ref=ref, kind=kind), patch.dict(os.environ, GITHUB_EVENT_NAME=event, GITHUB_REF_NAME=ref, GITHUB_REF_TYPE=kind, GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH='master'):
                with self.assertRaises(ValueError): daily.validate_invocation('canary')


if __name__ == '__main__':
    unittest.main()
