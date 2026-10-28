"""Validate signing-independent Windows compilation and CTest dispatch contracts."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


def step(text, name):
    match = re.search(r"(?m)^      - name: " + re.escape(name) + r"\n.*?(?=^      - name: |\Z)", text, re.S)
    if not match:
        raise AssertionError("Missing workflow step: " + name)
    return match.group(0)


class WindowsValidationWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.wrapper = (ROOT / ".github/workflows/validate-windows.yml").read_text()
        cls.reusable = (ROOT / ".github/workflows/ci-windows.yml").read_text()

    def test_opt_in_tests_preserve_artifact_only_dispatch(self):
        self.assertRegex(self.wrapper, r"build_tests:\n        description: [^\n]+\n        type: boolean\n        default: false")
        self.assertIn("build_tests: ${{ inputs.build_tests }}", self.wrapper)
        self.assertIn("build_only: true", self.wrapper)
        self.assertIn("release_commit: ${{ github.sha }}", self.wrapper)
        self.assertRegex(self.wrapper, r"use_self_hosted:\n        description: [^\n]+\n        type: boolean\n        default: false")
        self.assertNotIn("daily_unsigned:", self.wrapper)
        self.assertNotIn("secrets:", self.wrapper)
        self.assertNotIn("write", self.wrapper)
        self.assertNotIn("publish_", self.wrapper)

    def test_compilation_and_execution_share_the_test_input(self):
        build = step(self.reusable, "Build Windows")
        self.assertIn("-DBUILD_TESTS=${{ inputs.build_tests && 'ON' || 'OFF' }}", build)
        self.assertIn("cmake --build build --target all", build)
        execute = step(self.reusable, "Execute Windows tests")
        self.assertIn("if: inputs.build_tests", execute)
        self.assertNotIn("daily_unsigned", execute)
        self.assertIn("set -euo pipefail", execute)
        self.assertIn("ctest --test-dir build --output-on-failure --no-tests=error", execute)
        self.assertLess(self.reusable.index(build), self.reusable.index(execute))

    def test_artifact_signing_remains_daily_only(self):
        for name in ("Locate OpenSSL for artifact-only daily signing", "Self-sign only the daily gamepad catalog and setup tool"):
            signing = step(self.reusable, name)
            self.assertIn("if: inputs.daily_unsigned", signing)
            self.assertNotIn("if: inputs.build_tests", signing)
        self.assertIn("SIGNPATH_ENABLED: ${{ secrets.signpath_api_token != '' }}", self.reusable)


if __name__ == "__main__":
    unittest.main()
