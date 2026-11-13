"""Exercise the pinned Boost.Locale source-group correction with real CMake."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CMAKE = os.environ.get("CMAKE_COMMAND") or shutil.which("cmake")
PATCH = ROOT / "cmake/dependencies/patches/boost_locale_source_group.cmake"
ORIGINAL = 'source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/include/boost/locale" PREFIX "headers" FILES ${sources_inc})'


@unittest.skipUnless(CMAKE, "CMake is required")
class BoostLocalePatch(unittest.TestCase):
    def test_original_failure_patch_success_and_idempotence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            locale = root / "libs/locale"
            (locale / "include/boost/locale").mkdir(parents=True)
            (locale / "include/boost/locale.hpp").touch()
            (locale / "include/boost/locale/generator.hpp").touch()
            (root / "CMakeLists.txt").write_text('cmake_minimum_required(VERSION 3.16)\nproject(probe NONE)\nadd_subdirectory(libs/locale)\n')
            cmake_file = locale / "CMakeLists.txt"
            cmake_file.write_text('set(sources_inc include/boost/locale.hpp include/boost/locale/generator.hpp)\n' + ORIGINAL + '\nsource_group("headers" FILES "include/boost/locale.hpp")\n')
            def configure():
                return subprocess.run([CMAKE, "-S", str(root), "-B", str(root / "build")], capture_output=True, text=True)
            before = configure()
            if before.returncode != 0:
                self.assertIn("is not a prefix of file", before.stderr)
            # Some older CMake versions accept the invalid TREE; verify the
            # dependency invariant directly as well as corrected configuration.
            self.assertIn(ORIGINAL, cmake_file.read_text())
            command = [CMAKE, "-DBOOST_SOURCE_DIR=" + str(root), "-P", str(PATCH)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            patched = cmake_file.read_bytes()
            self.assertNotIn(ORIGINAL, patched.decode())
            self.assertIn(ORIGINAL.replace("/include/boost/locale", "/include/boost"), patched.decode())
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(cmake_file.read_bytes(), patched)
            after = configure()
            self.assertEqual(after.returncode, 0, after.stderr)

    def test_unrecognized_source_is_not_silently_patched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "libs/locale/CMakeLists.txt"
            target.parent.mkdir(parents=True)
            target.write_text("# Different upstream dependency\n")
            result = subprocess.run([CMAKE, "-DBOOST_SOURCE_DIR=" + str(root), "-P", str(PATCH)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Unexpected Boost.Locale source grouping", result.stderr)
            self.assertEqual(target.read_text(), "# Different upstream dependency\n")

    def test_all_fetchcontent_declarations_apply_patch(self):
        source = (ROOT / "cmake/dependencies/Boost_Sunshine.cmake").read_text()
        self.assertEqual(source.count("FetchContent_Declare("), 3)
        self.assertEqual(source.count("-DBOOST_SOURCE_DIR=<SOURCE_DIR>"), 3)
        self.assertEqual(source.count("/patches/boost_locale_source_group.cmake"), 3)


if __name__ == "__main__":
    unittest.main()
