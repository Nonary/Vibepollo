"""Check the actual macOS workflow resolver against the shared CMake guard."""
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MacOSVersionTests(unittest.TestCase):
    def resolve(self, requested, ref, tag):
        workflow = (ROOT / ".github/workflows/ci-macos.yml").read_text()
        step = workflow.split("      - name: Resolve build version\n", 1)[1]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1].split("\n      - name:", 1)[0])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "environment"
            env = dict(os.environ, INPUT_RELEASE_VERSION=requested, GITHUB_REF=ref,
                       GITHUB_REF_NAME=tag, GITHUB_ENV=str(output))
            subprocess.run(["bash", "-eu", "-c", script], env=env, check=True, capture_output=True)
            return output.read_text().strip().removeprefix("BUILD_VERSION=")

    def check_cmake(self, version, channel="", accepted=True):
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / "probe.cmake"
            probe.write_text('cmake_minimum_required(VERSION 3.25)\n'
                             f'set(BUILD_VERSION "{version}")\n'
                             f'set(BUILD_CHANNEL "{channel}")\n'
                             f'include("{ROOT}/cmake/prep/build_version.cmake")\n')
            env = os.environ.copy()
            env.pop("BUILD_VERSION", None)
            env.pop("BUILD_CHANNEL", None)
            result = subprocess.run(["cmake", "-P", str(probe)], env=env, capture_output=True, text=True)
            if accepted:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            else:
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("Version resolution produced 0.0.0", result.stderr)

    def test_resolver_cases(self):
        for requested, ref, tag, expected in [
            ("", "refs/heads/canary", "canary", "2.0.0-pr-validation"),
            ("", "refs/tags/v2.0.42", "v2.0.42", "2.0.42"),
            ("2.0.43", "refs/tags/v2.0.42", "v2.0.42", "2.0.43"),
            ("v2.0.44-beta.1", "refs/heads/canary", "canary", "2.0.44-beta.1"),
        ]:
            with self.subTest(requested=requested, ref=ref):
                actual = self.resolve(requested, ref, tag)
                self.assertEqual(actual, expected)
                self.check_cmake(actual)

    def test_allocated_numeric_channels(self):
        for channel in ("nightly", "stable", "prerelease"):
            self.check_cmake(self.resolve("2.0.45", "refs/heads/canary", "canary"), channel)

    def test_explicit_zero_still_rejected(self):
        self.check_cmake(self.resolve("0.0.0", "refs/heads/canary", "canary"), accepted=False)


if __name__ == "__main__":
    unittest.main()
