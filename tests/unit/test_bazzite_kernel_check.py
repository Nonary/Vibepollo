"""Exercise the compile-only host check with a fake kernel build and modinfo."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("bazzite_check", ROOT / "packaging/linux/bazzite/check-kernel.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class KernelCheckTests(unittest.TestCase):
    def test_kernel_validation(self):
        self.assertEqual(probe.checked_kernel("7.2.11-ogc4.1.fc44.x86_64"), "7.2.11-ogc4.1.fc44.x86_64")
        for release in ("6.15.9", "../../kernel", "7.2;id", "unknown", "7.2\n"):
            with self.subTest(release=release), self.assertRaises(ValueError):
                probe.checked_kernel(release)

    def test_source_integrity_and_traversal(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "file.c").write_bytes(b"source")
            manifest = {"files": {"file.c": hashlib.sha256(b"source").hexdigest()}}
            probe.verify_sources(root, manifest)
            (root / "file.c").write_bytes(b"changed")
            with self.assertRaises(ValueError):
                probe.verify_sources(root, manifest)
            with self.assertRaises(ValueError):
                probe.verify_sources(root, {"files": {"../file.c": "bad"}})

    def exercise_build(self, make_exit=0, vermagic="7.2.11 SMP", output=True):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source, kernel, result = [root / p for p in ("source", "kernel", "result")]
            for directory in (source, kernel, result):
                directory.mkdir()
            (source / "Makefile").touch()
            (kernel / ".config").write_text("CONFIG_CC_IS_CLANG=y\n")
            calls = []

            def fake_run(args, **kwargs):
                calls.append(args)
                self.assertEqual(args[0], "make")
                self.assertIn("LLVM=1", args)
                self.assertEqual(args[-1], "modules")
                if output:
                    (result / "vibeshine_drm/vibeshine_drm.ko").write_bytes(b"module")
                return type("Result", (), {"returncode": make_exit})()

            info = type("Info", (), {"stdout": vermagic})()
            with patch.object(probe.subprocess, "run", side_effect=fake_run), patch.object(probe, "run", return_value=info):
                report = probe.qualify(source, "vibeshine_drm", "7.2.11", kernel, result, 2)
            self.assertEqual(len(calls), 1)
            self.assertFalse((source / "vibeshine_drm_version.h").exists())
            return report

    def test_success_requires_matching_kernel(self):
        self.assertEqual(self.exercise_build()["status"], "compiled")
        self.assertEqual(self.exercise_build(vermagic="7.2.9 SMP")["status"], "failed")

    def test_failed_build_or_missing_artifact_is_not_success(self):
        self.assertEqual(self.exercise_build(make_exit=2)["status"], "failed")
        self.assertEqual(self.exercise_build(output=False)["status"], "failed")


if __name__ == "__main__":
    unittest.main()
