"""Exercise cache reuse and rejection without downloading/installing CUDA."""
import os
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "prepare-linux-cuda.sh"
BASH = shutil.which("bash") or r"C:\Program Files\Git\bin\bash.exe"


class CudaCacheTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        shutil.copyfile(SCRIPT, self.root / "prepare.sh")
        (self.root / "installer.sh").write_text('''#!/bin/bash
set -e
for arg; do
  case "$arg" in --toolkitpath=*) target=${arg#*=};; esac
done
mkdir -p "$target/bin"
printf '#!/bin/bash\necho "release 12.9, V12.9.86"\n' > "$target/bin/nvcc"
chmod +x "$target/bin/nvcc"
echo install >> installs
''')
        (self.root / "headers.patch").write_text("test patch\n")
        (self.root / "bin").mkdir()
        (self.root / "bin" / "patch").write_text("#!/bin/bash\ncat >/dev/null\necho patch >> patches\n")
        subprocess.run([BASH, "-c", "chmod +x bin/patch"], cwd=self.root, check=True)

    def run_prepare(self):
        env = dict(os.environ, CUDA_CACHE_DIR="cache")
        return subprocess.run(
            [BASH, "-c", 'export PATH="$PWD/bin:$PATH"; bash prepare.sh installer.sh destination headers.patch'],
            cwd=self.root, env=env, capture_output=True, text=True,
        )

    def test_cold_then_warm_reuses_verified_toolkit(self):
        self.assertEqual(self.run_prepare().returncode, 0)
        # Each hosted makepkg invocation starts with a fresh extracted source
        # destination. Git Bash may emulate ln with a directory copy on Windows.
        destination = self.root / "destination"
        if destination.is_dir() and not destination.is_symlink():
            shutil.rmtree(destination)
        else:
            destination.unlink()
        warm = self.run_prepare()
        self.assertEqual(warm.returncode, 0, warm.stderr)
        self.assertIn("Reusing exact verified", warm.stdout)
        self.assertEqual((self.root / "installs").read_text(), "install\n")
        self.assertEqual((self.root / "patches").read_text(), "patch\n")

    def test_changed_patch_rejects_cached_toolkit(self):
        self.assertEqual(self.run_prepare().returncode, 0)
        (self.root / "headers.patch").write_text("different patch\n")
        failure = self.run_prepare()
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("receipt mismatch", failure.stderr)

    def test_wrong_cached_compiler_rejects_entry(self):
        self.assertEqual(self.run_prepare().returncode, 0)
        (self.root / "cache" / "toolkit" / "bin" / "nvcc").write_text('#!/bin/bash\necho "release 13.0"\n')
        failure = self.run_prepare()
        self.assertNotEqual(failure.returncode, 0)
        self.assertIn("receipt mismatch", failure.stderr)


class InstallerOnlyJobTests(unittest.TestCase):
    """Two cold job toolkits share only a tiny checksum-pinned installer fixture."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.archive = self.root / "shared-installer.run"
        self.archive.write_text('''#!/bin/sh
set -eu
printf '%s\\n' "$@" > installer-arguments
for arg; do
  case "$arg" in --toolkitpath=*) target=${arg#*=};; esac
done
mkdir -p "$target/bin" "$target/include" "$target/lib64" "$target/nvvm/bin"
printf '#!/bin/sh\\necho "release 12.9, V12.9.86"\\n' > "$target/bin/nvcc"
chmod +x "$target/bin/nvcc"
printf 'unpatched\\n' > "$target/include/header.h"
printf 'runtime fixture\\n' > "$target/lib64/libcudart.so"
printf 'device compiler fixture\\n' > "$target/nvvm/bin/cicc"
echo install >> installs
''', encoding="utf-8", newline="\n")
        self.expected = hashlib.sha256(self.archive.read_bytes()).hexdigest()

    def job(self, name, archive=None):
        job = self.root / name
        job.mkdir()
        shutil.copyfile(archive or self.archive, job / "cuda-x86_64.run")
        shutil.copyfile(SCRIPT, job / "prepare.sh")
        (job / "installer.sha256").write_text(f"{self.expected}  cuda-x86_64.run\n", encoding="utf-8", newline="\n")
        (job / "headers.patch").write_text(
            "--- a/cuda/include/header.h\n+++ b/cuda/include/header.h\n"
            "@@ -1 +1 @@\n-unpatched\n+patched\n", encoding="utf-8", newline="\n")
        # This is the makepkg archive verification boundary, before prepare().
        result = subprocess.run(
            [BASH, "-c", "set -e; sha256sum --check installer.sha256; bash prepare.sh cuda-x86_64.run destination headers.patch"],
            cwd=job, env=dict(os.environ, CUDA_CACHE_DIR="scratch"), capture_output=True, text=True)
        return job, result

    def test_two_fresh_jobs_verify_installer_and_install_patch_complete_toolkit(self):
        for name in ("cold-job", "restored-installer-job"):
            with self.subTest(job=name):
                job, result = self.job(name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("cuda-x86_64.run: OK", result.stdout)
                self.assertNotIn("Reusing exact verified CUDA toolkit", result.stdout)
                self.assertEqual((job / "installs").read_text(), "install\n")
                toolkit = job / "scratch/toolkit"
                self.assertEqual((toolkit / "include/header.h").read_text(), "patched\n")
                self.assertTrue((toolkit / "lib64/libcudart.so").is_file())
                self.assertTrue((toolkit / "nvvm/bin/cicc").is_file())
                self.assertTrue((toolkit / ".verified-recipe").is_file())
                arguments = (job / "installer-arguments").read_text().splitlines()
                self.assertIn("--toolkit", arguments)
                self.assertNotIn("--toolkitcomponents", arguments)
        self.assertEqual(hashlib.sha256(self.archive.read_bytes()).hexdigest(), self.expected)
        # No full SDK content exists beside the shared cache input.
        self.assertFalse((self.root / "toolkit").exists())

    def test_modified_and_partial_archives_are_rejected_before_installation(self):
        original = self.archive.read_bytes()
        for name, data in (("modified", original + b"tampered\n"), ("partial", original[:80])):
            with self.subTest(archive=name):
                broken = self.root / (name + ".run")
                broken.write_bytes(data)
                job, result = self.job(name, broken)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("FAILED", result.stdout)
                self.assertFalse((job / "installs").exists())
                self.assertFalse((job / "scratch").exists())


if __name__ == "__main__":
    unittest.main()
