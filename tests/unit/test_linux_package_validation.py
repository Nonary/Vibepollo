"""Exercise exact-source seeding and trusted partial-cache eligibility with real Git."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = shutil.which("bash") or r"C:\Program Files\Git\bin\bash.exe"
ORIGIN = "https://github.com/Nonary/fixture.git"


class LinuxPackageValidationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.stage = Path(self.temp.name)
        for name in ("seed-linux-package-source.sh", "check-linux-partial-cache.sh"):
            shutil.copyfile(ROOT / "scripts" / name, self.stage / name)
        self.run_shell("git init -q source; git -C source config user.name Fixture; git -C source config user.email fixture@example.invalid; echo exact > source/file; git -C source add file; git -C source commit -qm exact; git -C source remote add origin " + ORIGIN, check=True)
        self.commit = self.run_shell("git -C source rev-parse HEAD", check=True).stdout.strip()
        self.tree = self.run_shell("git -C source rev-parse 'HEAD^{tree}'", check=True).stdout.strip()

    def run_shell(self, code, environment=None, check=False):
        return subprocess.run([BASH, "-c", code], cwd=self.stage, env=dict(os.environ, **(environment or {})), capture_output=True, text=True, check=check)

    def seed(self, commit=None, origin=ORIGIN):
        return self.run_shell(f"bash seed-linux-package-source.sh source mirror '{origin}' '{commit or self.commit}'")

    def test_seed_is_shallow_self_contained_and_exact(self):
        result = self.seed()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.run_shell("git --git-dir=mirror rev-parse HEAD", check=True).stdout.strip(), self.commit)
        self.assertEqual(self.run_shell("git --git-dir=mirror rev-parse 'HEAD^{tree}'", check=True).stdout.strip(), self.tree)
        self.assertEqual(self.run_shell("git --git-dir=mirror remote get-url origin", check=True).stdout.strip(), ORIGIN)
        self.assertTrue((self.stage / "mirror/shallow").is_file())
        self.assertFalse((self.stage / "mirror/objects/info/alternates").exists())
        self.run_shell("git --git-dir=mirror fsck --full", check=True)
        self.assertNotEqual(self.seed().returncode, 0)

    def test_seed_rejects_commit_origin_and_dirty_tree(self):
        self.assertNotEqual(self.seed(commit="f" * 40).returncode, 0)
        self.assertNotEqual(self.seed(origin="https://github.com/Nonary/other.git").returncode, 0)
        self.assertNotEqual(self.seed(origin="file://source").returncode, 0)
        (self.stage / "source/file").write_text("modified", encoding="utf-8")
        self.assertNotEqual(self.seed().returncode, 0)
        self.assertFalse((self.stage / "mirror").exists())

    def test_extracted_source_guard_precedes_submodule_preparation(self):
        self.assertEqual(self.seed().returncode, 0)
        self.run_shell("git clone -q --no-hardlinks mirror extracted", check=True)
        recipe = (ROOT / "packaging/linux/Arch/PKGBUILD").read_text(encoding="utf-8")
        prepare = "prepare() {" + recipe.split("prepare() {", 1)[1].split("\nbuild() {", 1)[0]
        code = "set -e\npkgname=extracted\n_use_cuda=false\n" + prepare + "\nprepare\n"
        valid = dict(_commit=self.commit, _validation_expected_tree=self.tree)
        self.assertEqual(self.run_shell(code, valid).returncode, 0)
        self.assertNotEqual(self.run_shell(code, dict(valid, _commit="f" * 40)).returncode, 0)
        self.assertNotEqual(self.run_shell(code, dict(valid, _validation_expected_tree="f" * 40)).returncode, 0)

    def prepare_cache(self):
        (self.stage / "installer").write_bytes(b"exact installer")
        (self.stage / "patch").write_bytes(b"exact patch")
        expected = hashlib.sha256(b"exact installer").hexdigest()
        (self.stage / "recipe").write_text(f"sha256sums_x86_64=('{expected}')\n", encoding="utf-8")
        self.run_shell("mkdir -p cuda/toolkit/bin compiler; printf '#!/bin/bash\\necho release 12.9,\\n' > cuda/toolkit/bin/nvcc; chmod +x cuda/toolkit/bin/nvcc; { sha256sum installer patch | cut -d' ' -f1; uname -m; } | sha256sum | cut -d' ' -f1 > cuda/toolkit/.verified-recipe; echo object > compiler/resultR; echo 'Result: cache_miss' > compiler.log", check=True)

    def partial(self, **overrides):
        environment = dict(VALIDATION_ONLY="true", GITHUB_EVENT_NAME="workflow_dispatch", EVENT_REPOSITORY="Nonary/fixture", GITHUB_REPOSITORY="Nonary/fixture", TOOLCHAIN_ABI="verified-abi", PACKAGE_BUILD_OUTCOME="failure", CUDA_CACHE_DIR="cuda", CCACHE_DIR="compiler")
        environment.update(overrides)
        return self.run_shell("bash check-linux-partial-cache.sh recipe installer patch compiler.log", environment)

    def test_failed_trusted_manual_work_retains_only_verified_caches(self):
        self.prepare_cache()
        self.assertEqual(self.partial().stdout.splitlines(), ["cuda=true", "compiler=true"])
        (self.stage / "cuda/toolkit/.verified-recipe").unlink()
        self.assertEqual(self.partial().stdout.splitlines(), ["compiler=true"])
        (self.stage / "compiler.log").unlink()
        self.assertEqual(self.partial().stdout, "")

    def test_partial_rejects_pr_wrong_repository_and_no_inputs(self):
        self.assertEqual(self.partial().stdout, "")
        self.prepare_cache()
        for changes in (dict(GITHUB_EVENT_NAME="pull_request"), dict(EVENT_REPOSITORY="fork/fixture"), dict(TOOLCHAIN_ABI=""), dict(PACKAGE_BUILD_OUTCOME="skipped"), dict(VALIDATION_ONLY="false")):
            with self.subTest(changes=changes):
                self.assertEqual(self.partial(**changes).stdout, "")
        (self.stage / "installer").write_bytes(b"incomplete")
        self.assertEqual(self.partial().stdout.splitlines(), ["compiler=true"])


class ComponentCompileGateTests(unittest.TestCase):
    def setUp(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("component_gate", ROOT / "scripts/build-linux-component-tests.py")
        self.helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.helper)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.build = Path(self.temp.name)
        self.reply = self.build / ".cmake/api/v1/reply"
        self.reply.mkdir(parents=True)

    def model(self, targets):
        import json
        (self.reply / "index-current.json").write_text(json.dumps({"reply": {"codemodel-v2": {"jsonFile": "model.json"}}}))
        (self.reply / "model.json").write_text(json.dumps({"configurations": [{"targets": [{"name": name} for name in targets]}]}))

    def test_all_configured_components_are_built_without_host_target(self):
        from unittest.mock import patch
        targets = ["sunshine", "test_component_linux_routed_link", "test_component_new_interface", "test_component_new_interface"]
        self.model(targets)
        with patch.object(self.helper.subprocess, "run") as build:
            self.helper.main(self.build)
        build.assert_called_once_with(["cmake", "--build", str(self.build), "--target", "test_component_linux_routed_link", "test_component_new_interface"], check=True)

    def test_absent_model_or_required_component_fails_closed(self):
        with self.assertRaises(ValueError):
            self.helper.component_targets(self.build)
        self.model(["sunshine", "test_component_new_interface"])
        with self.assertRaises(ValueError):
            self.helper.component_targets(self.build)

    def test_actual_component_compile_failure_propagates(self):
        from unittest.mock import patch
        self.model(["test_component_linux_routed_link"])
        with patch.object(self.helper.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "cmake")):
            with self.assertRaises(subprocess.CalledProcessError):
                self.helper.main(self.build)


if __name__ == "__main__":
    unittest.main()
