import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("compose_release_notes.py")


class ComposeReleaseNotesTest(unittest.TestCase):
    def compose(self, version, versions):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for entry in versions:
                (root / f"{entry}.md").write_text(
                    f"# {entry}\n\n## Fixes\n\n- Fix for {entry}\n", encoding="utf-8"
                )
            output = root / "output.txt"
            subprocess.run(
                [sys.executable, str(SCRIPT), "--release-version", version,
                 "--notes-dir", str(root), "--output", str(output)], check=True
            )
            return output.read_text(encoding="utf-8")

    def test_stable_includes_only_latest_previous_stable_patch(self):
        result = self.compose("2.0.3", ["2.0.1", "2.0.2", "2.0.2-stable.1",
                                      "2.0.2-beta.1", "2.0.3", "2.0.4", "1.9.9"])
        self.assertLess(result.index("## 2.0.3"), result.index("## 2.0.2\n"))
        self.assertIn("## 2.0.2-stable.1", result)
        for excluded in ("2.0.1", "2.0.2-beta.1", "2.0.4", "1.9.9"):
            self.assertNotIn(f"## {excluded}\n", result)

    def test_prerelease_remains_scoped_to_itself(self):
        result = self.compose("2.0.3-beta.1", ["2.0.2", "2.0.3-beta.1", "2.0.3"])
        self.assertIn("## 2.0.3-beta.1", result)
        self.assertNotIn("## 2.0.2\n", result)
        self.assertNotIn("## 2.0.3\n", result)

    def test_stable_respin_includes_current_line_and_previous_patch(self):
        result = self.compose("2.0.3-stable.1", ["2.0.2", "2.0.3", "2.0.3-stable.1", "2.0.3-stable.2"])
        for included in ("2.0.2", "2.0.3", "2.0.3-stable.1"):
            self.assertIn(f"## {included}\n", result)
        self.assertNotIn("## 2.0.3-stable.2\n", result)

    def test_first_patch_does_not_include_previous_minor(self):
        result = self.compose("2.1.0", ["2.0.9", "2.1.0"])
        self.assertNotIn("## 2.0.9\n", result)


if __name__ == "__main__":
    unittest.main()
