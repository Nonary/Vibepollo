"""Check the opt-in Bazzite RPM contract with CMake, without host mutation."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = os.environ.get("CMAKE_COMMAND") or shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake is required")
class BazzitePackageProfile(unittest.TestCase):
    def configure(self, enabled, extra=""):
        script = """cmake_minimum_required(VERSION 3.20)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_INSTALL_FULL_LIBDIR /usr/lib64)
set(CPACK_RPM_PACKAGE_REQUIRES libcap)
set(CPACK_RPM_USER_FILELIST original)
"""
        for hook in ("PRE_INSTALL", "POST_INSTALL", "PRE_UNINSTALL", "POST_UNINSTALL"):
            script += f'set(CPACK_RPM_{hook}_SCRIPT_FILE /unsafe/{hook})\n'
        script += f'set(SUNSHINE_BUILD_BAZZITE {"ON" if enabled else "OFF"})\n' + extra + "\n"
        script += f'include("{ROOT / "cmake/packaging/bazzite.cmake"}")\n'
        if enabled:
            for hook in ("PRE_INSTALL", "POST_INSTALL", "PRE_UNINSTALL", "POST_UNINSTALL"):
                script += f'if(DEFINED CPACK_RPM_{hook}_SCRIPT_FILE)\nmessage(FATAL_ERROR "Live hook retained")\nendif()\n'
            script += """
if(NOT CPACK_RPM_PACKAGE_REQUIRES MATCHES "vibepollo-kmod")
  message(FATAL_ERROR "Kernel package gate missing")
endif()
if(NOT CPACK_RPM_USER_FILELIST MATCHES "4755,root,root")
  message(FATAL_ERROR "KWin bridge payload mode missing")
endif()
if(NOT CPACK_GENERATOR STREQUAL "RPM")
  message(FATAL_ERROR "Unexpected generator")
endif()
"""
        else:
            script += """
if(NOT CPACK_RPM_POST_INSTALL_SCRIPT_FILE STREQUAL "/unsafe/POST_INSTALL")
  message(FATAL_ERROR "Changed mutable-host default")
endif()
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "check.cmake"
            path.write_text(script)
            return subprocess.run([CMAKE, "-P", str(path)], capture_output=True, text=True)

    def test_removes_all_live_hooks_and_requires_driver_package(self):
        result = self.configure(True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_default_keeps_existing_installation_contract(self):
        result = self.configure(False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejects_non_native_profiles(self):
        for extra in ("set(CMAKE_SYSTEM_NAME Windows)", "set(SUNSHINE_BUILD_STEAMOS ON)",
                      "set(SUNSHINE_BUILD_FLATPAK ON)", "set(SUNSHINE_BUILD_APPIMAGE ON)"):
            with self.subTest(extra=extra):
                result = self.configure(True, extra)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("requires a native Linux RPM build", result.stderr)


if __name__ == "__main__":
    unittest.main()
