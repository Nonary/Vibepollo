import unittest
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]


class ValidateWindowsNumericAnchorTests(unittest.TestCase):
    def test_numeric_nightly_qualification_resolves_and_passes_stable_anchor(self):
        workflow = yaml.safe_load((ROOT / ".github/workflows/validate-windows.yml").read_text())
        prepare = workflow["jobs"]["prepare"]
        self.assertEqual(prepare["permissions"], {"contents": "read"})
        self.assertEqual(prepare["outputs"]["stable_anchor_tag"],
                         "${{ steps.identity.outputs.stable_anchor_tag }}")
        self.assertEqual(prepare["outputs"]["stable_anchor_commit"],
                         "${{ steps.identity.outputs.stable_anchor_commit }}")
        script = next(step["run"] for step in prepare["steps"]
                      if step.get("name") == "Resolve numeric nightly stable anchor")
        self.assertIn('re.fullmatch(r"[0-9]+\\.[0-9]+\\.[0-9]+"', script)
        self.assertIn("latest_stable_release(releases)", script)
        self.assertIn('"git", "merge-base", "--is-ancestor"', script)
        self.assertIn('Numeric nightly qualification release_tag must equal release_version', script)
        self.assertLess(script.index("if numeric_nightly:"), script.index("release_pages("),
                        "Legacy suffix qualification must not resolve or require the numeric stable anchor")
        self.assertNotIn('git tag', script)
        self.assertNotIn('git push', script)

        windows = workflow["jobs"]["windows"]
        call = windows["with"]
        self.assertEqual(windows["needs"], "prepare")
        self.assertEqual(call["stable_anchor_tag"], "${{ needs.prepare.outputs.stable_anchor_tag }}")
        self.assertEqual(call["stable_anchor_commit"], "${{ needs.prepare.outputs.stable_anchor_commit }}")
        self.assertEqual(call["release_channel"], "${{ needs.prepare.outputs.numeric_nightly == 'true' && 'nightly' || '' }}")
        self.assertEqual(call["daily_validation_only"], "${{ needs.prepare.outputs.numeric_nightly == 'true' }}")
        self.assertEqual(call["release_tag"], "${{ inputs.release_tag || (needs.prepare.outputs.numeric_nightly == 'true' && inputs.release_version) || '' }}")
        self.assertIn("|| ''", call["release_tag"], "Legacy suffix qualification keeps an empty release_tag when omitted")


if __name__ == "__main__":
    unittest.main()
