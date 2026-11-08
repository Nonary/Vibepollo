"""Bind the typed packaging expectation to the explicitly audited producer lock."""
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DisplayPackagingReleaseLock(unittest.TestCase):
    def test_audited_release_identity_and_cpp_expectation(self):
        lock = json.loads((ROOT / "third-party/libvirtualdisplay-release/windows-x64.json").read_text())
        self.assertEqual(lock["repository"], "Nonary/libvirtualdisplay")
        self.assertEqual(lock["tag"], "v1.6.5")
        self.assertEqual(lock["source_revision"], "b6874684d82925d0d76f487d8e4ba3ec71c9721a")
        self.assertEqual(lock["archive_sha256"], "e37967d86c0102903bcff0bc40c5aece6653686165276c5e1045a7e11aad522d")
        self.assertEqual(lock["driver_ver"], "10/29/2026,1.6.5.0")
        source = (ROOT / "tests/integration/test_virtual_display_packaging.cpp").read_text()
        tags = re.findall(r'EXPECT_EQ\(contract::libvirtualdisplay_release_tag, "([^"]+)"\);', source)
        self.assertEqual(tags, [lock["tag"]])

    def test_actual_cmake_contract_consumes_the_audited_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "contract.cmake"
            script.write_text(
                f'include("{ROOT.as_posix()}/cmake/packaging/windows_virtual_display_contract.cmake")\n'
                'if(NOT SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY STREQUAL "Nonary/libvirtualdisplay" OR '
                'NOT SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG STREQUAL "v1.6.5")\n'
                '  message(FATAL_ERROR "Packaging contract does not consume audited v1.6.5")\n'
                'endif()\n')
            subprocess.run(["cmake", "-P", str(script)], check=True)


if __name__ == "__main__":
    unittest.main()
