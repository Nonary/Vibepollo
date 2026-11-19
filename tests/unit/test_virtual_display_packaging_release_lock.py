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

    def test_checkout_keeps_the_windows_producer_and_linux_bridge_pins_separate(self):
        tracked = subprocess.check_output(
            ["git", "ls-files", "--stage", "--", "third-party/libvirtualdisplay", "third-party/libvirtualdisplay-linux"],
            cwd=ROOT, text=True,
        )
        pins = {}
        for row in tracked.splitlines():
            attributes, path = row.split("\t", 1)
            mode, revision, stage = attributes.split()
            self.assertEqual((mode, stage), ("160000", "0"))
            pins[path] = revision
        lock = json.loads((ROOT / "third-party/libvirtualdisplay-release/windows-x64.json").read_text())
        self.assertEqual(pins["third-party/libvirtualdisplay"], lock["source_revision"])
        self.assertNotEqual(pins["third-party/libvirtualdisplay-linux"], lock["source_revision"])

    def test_cmake_selects_the_source_for_each_target(self):
        module = ROOT / "cmake/dependencies/libvirtualdisplay_source.cmake"
        with tempfile.TemporaryDirectory() as directory:
            checkout = Path(directory)
            windows_source = checkout / "third-party/libvirtualdisplay"
            linux_source = checkout / "third-party/libvirtualdisplay-linux"
            for source in (windows_source, linux_source):
                source.mkdir(parents=True, exist_ok=True)
                (source / "CMakeLists.txt").touch()
            uapi = linux_source / "linux/vibeshine-drm/vibeshine_drm_uapi.h"
            uapi.parent.mkdir(parents=True)
            uapi.touch()
            script = checkout / "resolve.cmake"
            script.write_text(
                "function(add_library)\nendfunction()\n"
                "function(target_include_directories)\nendfunction()\n"
                f'include("{module.as_posix()}")\n'
                'if(NOT SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR STREQUAL EXPECTED_SOURCE)\n'
                '  message(FATAL_ERROR "Wrong libvirtualdisplay checkout: ${SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR}")\n'
                'endif()\n'
            )
            for system, source in (("Windows", windows_source), ("Linux", linux_source)):
                with self.subTest(system=system):
                    subprocess.run(
                        ["cmake", f"-DCMAKE_SYSTEM_NAME={system}", f"-DEXPECTED_SOURCE={source.as_posix()}",
                         "-P", str(script)], cwd=checkout, check=True,
                    )

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
