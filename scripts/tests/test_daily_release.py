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


class DailyTests(unittest.TestCase):
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
        self.assertEqual(first["tag"], "daily-2026-10-11")
        self.assertEqual(first["version"], "2.0.0-daily.d20261011+aaaaaaaaaaaa")
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
        for retired in ("vibe", "vibe-test"):
            with self.assertRaises(ValueError):
                daily.validate_source("Nonary/vibeshine", retired, f"Nonary/vibeshine:{retired}")

    def test_migration_confirmation_pins_both_exact_sources(self):
        upstream = "b" * 40
        expected = f"Nonary/vibeshine:{upstream}:Nonary/Vibepollo:{SHA}"
        daily.validate_migration("Nonary/Vibepollo", SHA, upstream, expected)
        for target, source, confirmation in ((SHA, upstream, ""), ("c" * 40, upstream, expected), (SHA, "c" * 40, expected), ("HEAD", upstream, expected)):
            with self.assertRaises(ValueError):
                daily.validate_migration("Nonary/Vibepollo", target, source, confirmation)

    def test_remote_tag_and_draft_collisions_are_immutable(self):
        for tag_output, releases in (("a refs/tags/daily", [[]]), ("", [[{"tag_name": daily.identity(SHA, dt.datetime.now(dt.timezone.utc).date(), "2.0.0")["tag"], "draft": True}]])):
            with tempfile.TemporaryDirectory() as temp:
                with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(Path(temp) / "output")):
                    with patch.object(daily, "run", side_effect=["", SHA, "", json.dumps(releases), "", "2.0.0-beta.4\n2.0.0", tag_output]):
                        with self.assertRaises(ValueError):
                            daily.resolve()

    def test_resolve_fetches_confirmed_remote_commit_and_emits_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", "", "2.0.0-beta.4\n2.0.0", ""]) as commands:
                    daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("version=2.0.0-daily", output.read_text())
            self.assertIn("should_build=true", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, ("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/daily-source"))

    def test_unchanged_repo_skips_before_date_and_upstream_migration_checks(self):
        for repository in ("Nonary/vibeshine", "Nonary/Vibepollo"):
            with self.subTest(repository=repository), tempfile.TemporaryDirectory() as temp:
                output = Path(temp) / "output"
                with patch.dict(os.environ, GITHUB_REPOSITORY=repository, GITHUB_SHA=SHA,
                                DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                                DAILY_RELEASE_SOURCE_CONFIRMED=f"{repository}:master", GITHUB_OUTPUT=str(output),
                                DAILY_RELEASE_MIGRATION_CONFIRMED="stale upstream pin"):
                    with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]"]) as commands:
                        with patch.object(daily, "previous_successful_nightly", return_value={"tag": "daily-2026-10-10", "source_commit": SHA}) as baseline:
                            daily.resolve()
                baseline.assert_called_once_with(repository, [[]])
                self.assertIn("should_build=false", output.read_text())
                self.assertNotIn("version=", output.read_text())
                self.assertFalse(any("ls-remote" in call.args for call in commands.call_args_list))

    def test_changed_target_still_requires_exact_migration_confirmation(self):
        with tempfile.TemporaryDirectory() as temp:
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/Vibepollo", GITHUB_SHA=SHA,
                            DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master",
                            DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/Vibepollo:master", GITHUB_OUTPUT=str(Path(temp) / "output"),
                            DAILY_RELEASE_MIGRATION_CONFIRMED="stale upstream pin"):
                with patch.object(daily, "run", side_effect=["", SHA, "", "[[]]", f"{'b' * 40} refs/heads/master"]):
                    with patch.object(daily, "previous_successful_nightly", return_value={"tag": "daily-2026-10-10", "source_commit": "c" * 40}):
                        with self.assertRaisesRegex(ValueError, "migration"):
                            daily.resolve()

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


if __name__ == "__main__":
    unittest.main()
