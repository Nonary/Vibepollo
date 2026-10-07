#!/usr/bin/env python3
"""Exercise release GPU target selection without requiring a CUDA GPU/toolkit."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CudaReleasePolicy(unittest.TestCase):
    def configure(self, version, required=True):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "policy.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                f'set(CMAKE_CUDA_COMPILER_VERSION "{version}")\n'
                f'set(SUNSHINE_REQUIRE_CUDA_PASCAL {"ON" if required else "OFF"})\n'
                f'include("{ROOT}/cmake/compile_definitions/cuda_architectures.cmake")\n'
                'message(STATUS "TARGETS=${CMAKE_CUDA_ARCHITECTURES}")\n'
            )
            return subprocess.run(
                ["cmake", "-P", str(script)], capture_output=True, text=True
            )

    def test_release_retains_pascal_and_modern_targets(self):
        result = self.configure("12.9.86")
        self.assertEqual(result.returncode, 0, result.stderr)
        targets = result.stdout.split("TARGETS=", 1)[1].strip().split(";")
        for target in ("50", "61", "70", "75", "89", "120"):
            self.assertIn(target, targets)

    def test_release_rejects_incompatible_toolkits(self):
        for version in ("12.8.93", "13.0.88", "13.3.73"):
            with self.subTest(version=version):
                result = self.configure(version)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Release packages require CUDA 12.9", result.stderr)

    def test_nonrelease_build_can_use_cuda_13(self):
        result = self.configure("13.3.73", required=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        targets = result.stdout.split("TARGETS=", 1)[1].strip().split(";")
        self.assertNotIn("61", targets)
        self.assertIn("120", targets)

    def test_shared_builder_uses_pin_and_rejects_stale_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            # Load real functions without executing distro detection/install.
            functions = stage / "functions.sh"
            functions.write_text((ROOT / "scripts/linux_build.sh").read_text().split(
                "# Determine the OS and call the appropriate function", 1)[0])
            nvcc = stage / "build/cuda/bin/nvcc"
            nvcc.parent.mkdir(parents=True)
            script = r'''
source "$FUNCTIONS"
build_dir="$PWD/build"
gcc_version=14
cmake() { printf '%s\n' "$@"; }
check_version() { return 1; }
# Any attempt to auto-detect a system toolkit should fail this test.
detect_nvcc_path() { echo 'unexpected system CUDA lookup' >&2; exit 99; }
run_step_cmake
'''
            for release in ("12.9", "13.3"):
                nvcc.write_text(f"#!/bin/sh\necho 'Cuda compilation tools, release {release}, V{release}.86'\n")
                nvcc.chmod(0o755)
                result = subprocess.run(
                    ["bash", "-c", script], cwd=stage,
                    env=dict(os.environ, FUNCTIONS=str(functions)),
                    capture_output=True, text=True)
                if release == "12.9":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn(f"-DCMAKE_CUDA_COMPILER:PATH={nvcc}", result.stdout)
                    self.assertIn("-DSUNSHINE_REQUIRE_CUDA_PASCAL=ON", result.stdout)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("incompatible toolkit", result.stderr)

    def test_arch_selects_private_toolkit_even_with_system_cuda(self):
        with tempfile.TemporaryDirectory() as directory:
            # Execute the real PKGBUILD build function, stubbing only external
            # tools, to observe the arguments passed to CMake.
            script = '''
set -e
source "$REPO/packaging/linux/Arch/PKGBUILD"
cmake() { printf '%s\\n' "$@"; }
appstreamcli() { :; }
appstream-util() { :; }
desktop-file-validate() { :; }
build
'''
            for enabled in ("true", "false"):
                env = dict(os.environ, REPO=str(ROOT), srcdir=directory,
                           _use_cuda=enabled, CUDA_PATH="/opt/cuda-13",
                           NVCC_CCBIN="/usr/bin/g++-15")
                result = subprocess.run(["bash", "-c", script], env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                if enabled == "true":
                    self.assertIn(f"-DCMAKE_CUDA_COMPILER={directory}/cuda/bin/nvcc", result.stdout)
                    self.assertIn("-DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-14", result.stdout)
                    self.assertIn("-DSUNSHINE_REQUIRE_CUDA_PASCAL=ON", result.stdout)
                else:
                    self.assertIn("-DSUNSHINE_ENABLE_CUDA=OFF", result.stdout)
                    self.assertNotIn("-DCMAKE_CUDA_COMPILER=", result.stdout)


if __name__ == "__main__":
    unittest.main()
