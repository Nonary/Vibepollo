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
        self.assertEqual(lock["tag"], "v1.6.4")
        self.assertEqual(lock["source_revision"], "2a29ea71a52d15522c95b3de532b6612c54d75cc")
        self.assertEqual(lock["archive_sha256"], "34c6cddb507187b9a0971765644b38637004d7ebed104e830363f36b661ed075")
        self.assertEqual(lock["driver_ver"], "10/04/2026,1.6.4.0")
        source = (ROOT / "tests/integration/test_virtual_display_packaging.cpp").read_text()
        tags = re.findall(r'EXPECT_EQ\(contract::libvirtualdisplay_release_tag, "([^"]+)"\);', source)
        self.assertEqual(tags, [lock["tag"]])

    def test_actual_cmake_contract_consumes_the_audited_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            script = Path(directory) / "contract.cmake"
            script.write_text(
                f'include("{ROOT.as_posix()}/cmake/packaging/windows_virtual_display_contract.cmake")\n'
                'if(NOT SUNSHINE_VDD_LIBVIRTUALDISPLAY_REPOSITORY STREQUAL "Nonary/libvirtualdisplay" OR '
                'NOT SUNSHINE_VDD_LIBVIRTUALDISPLAY_RELEASE_TAG STREQUAL "v1.6.4")\n'
                '  message(FATAL_ERROR "Packaging contract does not consume audited v1.6.4")\n'
                'endif()\n')
            subprocess.run(["cmake", "-P", str(script)], check=True)


if __name__ == "__main__":
    unittest.main()
