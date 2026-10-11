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
                    with patch.object(daily, "run", side_effect=["", SHA, "", "", "2.0.0-beta.4\n2.0.0", tag_output, json.dumps(releases)]):
                        with self.assertRaises(ValueError):
                            daily.resolve()

    def test_resolve_fetches_confirmed_remote_commit_and_emits_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "output"
            with patch.dict(os.environ, GITHUB_REPOSITORY="Nonary/vibeshine", GITHUB_SHA=SHA, DAILY_PRIMARY_BRANCH="master", DAILY_RELEASE_SOURCE_BRANCH="master", DAILY_RELEASE_SOURCE_CONFIRMED="Nonary/vibeshine:master", GITHUB_OUTPUT=str(output)):
                with patch.object(daily, "run", side_effect=["", SHA, "", "", "2.0.0-beta.4\n2.0.0", "", "[[]]"]) as commands:
                    daily.resolve()
            self.assertIn(f"commit={SHA}", output.read_text())
            self.assertIn("version=2.0.0-daily", output.read_text())
            self.assertEqual(commands.call_args_list[0].args, ("git", "fetch", "--no-tags", "origin", "refs/heads/master:refs/remotes/origin/daily-source"))

    def test_finalized_assets_reject_corruption_wrong_run_and_extra_files(self):
        info = daily.identity(SHA, dt.date(2026, 10, 11), "2.0.0")
        for scenario in ("valid", "corrupt", "wrong-run", "wrong-source", "extra", "missing"):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                (root / "VibeshineSetup.exe").write_bytes(b"setup")
                (root / "Vibeshine.msi").write_bytes(b"msi")
                provenance = {"schema_version": 1, "release_tag": info["tag"], "release_commit": SHA, "source_build_run_id": "123", "assets": {"VibeshineSetup.exe": hashlib.sha256(b"setup").hexdigest()}}
                if scenario == "corrupt":
                    (root / "VibeshineSetup.exe").write_bytes(b"tampered")
                if scenario == "wrong-run":
                    provenance["source_build_run_id"] = "456"
                if scenario == "wrong-source":
                    provenance["release_commit"] = "b" * 40
                if scenario == "extra":
                    (root / "local-driver.cat").write_bytes(b"untrusted")
                if scenario == "missing":
                    (root / "Vibeshine.msi").unlink()
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
