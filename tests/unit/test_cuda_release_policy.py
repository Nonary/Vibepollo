#!/usr/bin/env python3
"""Exercise release GPU target selection without requiring a CUDA GPU/toolkit."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BASH = r"C:\Program Files\Git\bin\bash.exe" if os.name == "nt" else "bash"


class CudaReleasePolicy(unittest.TestCase):
    def configure(self, version, required=True):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "policy.cmake"
            script.write_text(
                'cmake_minimum_required(VERSION 3.25)\n'
                f'set(CMAKE_CUDA_COMPILER_VERSION "{version}")\n'
                f'set(SUNSHINE_REQUIRE_CUDA_PASCAL {"ON" if required else "OFF"})\n'
                f'include("{ROOT.as_posix()}/cmake/compile_definitions/cuda_architectures.cmake")\n'
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
                    [BASH, "-c", script], cwd=stage,
                    env=dict(os.environ, FUNCTIONS=functions.as_posix()),
                    capture_output=True, text=True)
                if release == "12.9":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    shell_stage = subprocess.run([BASH, "-c", "pwd -P"], cwd=stage, capture_output=True, text=True, check=True).stdout.strip()
                    self.assertIn(f"-DCMAKE_CUDA_COMPILER:PATH={shell_stage}/build/cuda/bin/nvcc", result.stdout)
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
cmake() {
  if [[ "$1" == --build ]]; then echo FULL-GATE; else echo CONFIGURE-GATE; fi
  printf '%s\\n' "$@"
}
# This policy fixture does not have configured component source/dependencies.
# Stub only the external helper and verify its actual invocation and order.
python3() {
  [[ "$1" == "$srcdir/$pkgname/scripts/build-linux-component-tests.py" && "$2" == build ]] || return 97
  echo COMPONENT-GATE
}
appstreamcli() { :; }
appstream-util() { :; }
desktop-file-validate() { :; }
build
'''
            for enabled in ("true", "false"):
                fixture_src = Path(directory).as_posix()
                env = dict(os.environ, REPO=ROOT.as_posix(), srcdir=fixture_src, _build_tests="ON",
                           _use_cuda=enabled, CUDA_PATH="/opt/cuda-13",
                           NVCC_CCBIN="/usr/bin/g++-15")
                result = subprocess.run([BASH, "-c", script], cwd=directory, env=env,
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                gates = [line for line in result.stdout.splitlines() if line.endswith("-GATE")]
                self.assertEqual(gates, ["CONFIGURE-GATE", "COMPONENT-GATE", "FULL-GATE"])
                if enabled == "true":
                    self.assertIn(f"-DCMAKE_CUDA_COMPILER={fixture_src}/cuda/bin/nvcc", result.stdout)
                    self.assertIn("-DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++-14", result.stdout)
                    self.assertIn("-DSUNSHINE_REQUIRE_CUDA_PASCAL=ON", result.stdout)
                else:
                    self.assertIn("-DSUNSHINE_ENABLE_CUDA=OFF", result.stdout)
                    self.assertNotIn("-DCMAKE_CUDA_COMPILER=", result.stdout)

    def test_final_pascal_ctest_and_package_gates_still_run_in_order(self):
        # Execute the real recipe check/package functions with tiny external
        # tool fixtures. A failed Pascal or CTest gate must block packaging.
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "build").mkdir()
            (stage / "cuda/bin").mkdir(parents=True)
            (stage / "build/vibepollo").write_text(
                '#!/bin/sh\necho version >> "$GATE_LOG"\necho fixture-version\n')
            (stage / "cuda/bin/cuobjdump").write_text(
                '#!/bin/sh\necho pascal >> "$GATE_LOG"\nprintf "%s\\n" "$DEVICE_ELF"\n')
            script = '''
set -e
export GATE_LOG="$PWD/gates"
chmod +x build/vibepollo cuda/bin/cuobjdump
source "$REPO/packaging/linux/Arch/PKGBUILD"
srcdir="$PWD"
pkgdir="$PWD/pkg"
ctest() { echo ctest >> "$GATE_LOG"; return "$CTEST_RESULT"; }
cmake() { [[ "$1" == --install && "$2" == build ]]; echo package >> "$GATE_LOG"; }
check
cd "$srcdir"
package
'''
            for name, elf, ctest_result, gates in (
                    ("success", "fixture.sm_61.cubin", "0", ["version", "pascal", "ctest", "package"]),
                    ("missing-pascal", "fixture.sm_89.cubin", "0", ["version", "pascal"]),
                    ("failed-ctest", "fixture.sm_61.cubin", "1", ["version", "pascal", "ctest"])):
                with self.subTest(gate=name):
                    (stage / "gates").unlink(missing_ok=True)
                    result = subprocess.run([BASH, "-c", script], cwd=stage,
                                            env=dict(os.environ, REPO=ROOT.as_posix(), CARCH="x86_64",
                                                     _use_cuda="true", _build_tests="ON",
                                                     DEVICE_ELF=elf, CTEST_RESULT=ctest_result),
                                            capture_output=True, text=True)
                    self.assertEqual((stage / "gates").read_text().splitlines(), gates)
                    if name == "success":
                        self.assertEqual(result.returncode, 0, result.stderr)
                    else:
                        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
