"""Exercise cache reuse and rejection without downloading/installing CUDA."""
import os
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


if __name__ == "__main__":
    unittest.main()
