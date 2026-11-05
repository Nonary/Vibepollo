"""Validate signing-independent Windows validation and nightly qualification."""
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
        cls.production_signing = (ROOT / ".github/workflows/sign-release.yml").read_text()

    def test_standard_validation_tests_are_opt_in_and_signing_independent(self):
        self.assertRegex(self.wrapper, r"build_tests:\n        description: [^\n]+\n        type: boolean\n        default: false")
        self.assertIn("build_tests: ${{ inputs.qualify_nightly_installer || inputs.build_tests }}", self.wrapper)
        self.assertIn("build_only: ${{ !inputs.qualify_nightly_installer }}", self.wrapper)
        self.assertIn("daily_unsigned: ${{ inputs.qualify_nightly_installer }}", self.wrapper)
        self.assertNotIn("signpath_api_token:", self.wrapper)
        self.assertNotIn("SIGNPATH_API_TOKEN", self.wrapper)

        # The ordinary validation path passes empty scoped secrets. Enabling
        # build_tests alone must not turn on nightly qualification or signing.
        self.assertIn("inputs.qualify_nightly_installer && secrets.TRUEHDR_RUNTIME_TOKEN || ''", self.wrapper)
        self.assertIn("inputs.qualify_nightly_installer && secrets.NIGHTLY_GAMEPAD_PFX_BASE64 || ''", self.wrapper)
        self.assertIn("inputs.qualify_nightly_installer && secrets.NIGHTLY_GAMEPAD_PFX_PASSWORD || ''", self.wrapper)
        self.assertNotIn("contents: write", self.wrapper)

        execute = step(self.reusable, "Execute Windows tests")
        self.assertIn("if: inputs.build_tests", execute)
        self.assertIn("ctest --test-dir build --output-on-failure --no-tests=error", execute)

    def test_nightly_qualification_forces_tests_and_fresh_unsigned_packaging(self):
        self.assertRegex(self.wrapper, r"qualify_nightly_installer:\n        description: [^\n]+\n        type: boolean\n        default: false")
        self.assertIn("build_tests: ${{ inputs.qualify_nightly_installer || inputs.build_tests }}", self.wrapper)
        self.assertIn("build_only: ${{ !inputs.qualify_nightly_installer }}", self.wrapper)
        self.assertIn("daily_unsigned: ${{ inputs.qualify_nightly_installer }}", self.wrapper)
        self.assertIn("require_truehdr_runtime: ${{ inputs.qualify_nightly_installer }}", self.wrapper)

        policy = step(self.reusable, "Enforce unsigned daily build policy")
        self.assertIn("if: inputs.daily_unsigned", policy)
        self.assertIn("$env:SIGNPATH_ENABLED -ne 'false'", policy)
        self.assertIn("$env:BUILD_TESTS -ne 'true'", policy)
        self.assertIn("$env:BUILD_ONLY -ne 'false'", policy)
        self.assertIn("$env:ARTIFACT_SOURCE_RUN_ID -ne ''", policy)
        self.assertIn("test_daily_version_contract.cmake", policy)
        self.assertIn("test_daily_gamepad_signing.ps1", policy)

        execute = step(self.reusable, "Execute Windows tests")
        self.assertIn("if: inputs.build_tests", execute)
        self.assertIn("ctest --test-dir build --output-on-failure --no-tests=error", execute)

    def test_nightly_qualification_resolves_read_only_stable_anchor(self):
        anchor = re.search(r"(?ms)^  nightly-anchor:\n.*?(?=^  windows:)", self.wrapper)
        self.assertIsNotNone(anchor, "Nightly qualification must resolve its stable anchor first")
        anchor = anchor.group(0)
        self.assertIn("needs: [classify-qualification]", anchor)
        self.assertIn("needs.classify-qualification.outputs.nightly_qualification == 'true'", anchor)
        self.assertIn("contents: read", anchor)
        self.assertIn("Resolve exact published stable anchor", anchor)
        self.assertIn("python scripts/daily_release.py stable-anchor", anchor)
        self.assertNotIn("contents: write", anchor)

        classify = re.search(r"(?ms)^  classify-qualification:\n.*?(?=^  nightly-anchor:)", self.wrapper)
        self.assertIsNotNone(classify, "The numeric versus legacy identity must be resolved first")
        classify = classify.group(0)
        self.assertIn("permissions: {}", classify)
        self.assertIn("Classify numeric versus legacy qualification", classify)
        self.assertIn(r"re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', os.environ['RELEASE_VERSION'])", classify)
        self.assertIn("daily_validation_only={str(numeric).lower()}", classify)

        windows = re.search(r"(?ms)^  windows:\n.*?(?=\Z)", self.wrapper).group(0)
        self.assertIn("needs: [classify-qualification, nightly-anchor]", windows)
        self.assertIn("needs.nightly-anchor.result == 'success'", windows)
        self.assertIn("daily_validation_only: ${{ needs.classify-qualification.outputs.daily_validation_only }}", windows)
        self.assertIn("stable_anchor_tag: ${{ needs.nightly-anchor.outputs.stable_anchor_tag }}", windows)
        self.assertIn("stable_anchor_commit: ${{ needs.nightly-anchor.outputs.stable_anchor_commit }}", windows)

    def test_legacy_validation_keeps_non_nightly_identity_and_skips_anchor(self):
        windows = re.search(r"(?ms)^  windows:\n.*?(?=\Z)", self.wrapper).group(0)
        self.assertIn("needs.classify-qualification.outputs.nightly_qualification != 'true' || needs.nightly-anchor.result == 'success'", windows)
        self.assertIn("release_channel: ${{ inputs.qualify_nightly_installer && 'nightly' || 'legacy' }}", windows)
        self.assertIn("release_tag: ${{ inputs.release_tag || (needs.classify-qualification.outputs.daily_validation_only == 'true' && inputs.release_version || '') }}", windows)
        self.assertIn("daily_validation_only: ${{ needs.classify-qualification.outputs.daily_validation_only }}", windows)
        classify = re.search(r"(?ms)^  classify-qualification:\n.*?(?=^  nightly-anchor:)", self.wrapper).group(0)
        self.assertIn("numeric = os.environ['QUALIFY_NIGHTLY'] == 'true' and bool(", classify)
        self.assertIn("nightly_qualification={str(numeric).lower()}", classify)
        # A qualified legacy version can run the mandatory daily safeguards,
        # while only a dotted numeric version receives an anchor and numeric tag.
        self.assertIn("build_tests: ${{ inputs.qualify_nightly_installer || inputs.build_tests }}", windows)
        self.assertIn("daily_unsigned: ${{ inputs.qualify_nightly_installer }}", windows)

    def test_signing_is_confined_to_nightly_and_production_signing_workflow(self):
        daily_gamepad = step(self.reusable, "Self-sign only the daily gamepad catalog and setup tool")
        self.assertIn("if: inputs.daily_unsigned", daily_gamepad)
        self.assertIn("secrets.daily_gamepad_pfx_base64", daily_gamepad)
        self.assertIn("secrets.daily_gamepad_pfx_password", daily_gamepad)
        self.assertNotIn("inputs.build_tests", daily_gamepad)

        self.assertIn("require_signpath_signing: true", self.production_signing)
        self.assertIn("signpath_api_token: ${{ secrets.SIGNPATH_API_TOKEN }}", self.production_signing)
        self.assertNotIn("signpath_api_token:", self.wrapper)


if __name__ == "__main__":
    unittest.main()
