"""Check the DualSense production-header edge with modular Boost, not unified headers.

Set BOOST_MODULAR_ARCHIVE to the pinned 1.89.0 archive and CXX to a compiler to
run the positive/negative syntax probes. Only the format interface closure is
extracted into a temporary directory; the repository's real stat_trackers.h is
compiled. A poison fallback prevents a system Boost installation masking the
missing imported-target include directory.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
ARCHIVE_SHA256 = "67acec02d0d118b5de9eb441f5fb707b3a1cdd884be00ca24b9a73c995511f74"


def registration():
    text = (ROOT / "tests/CMakeLists.txt").read_text()
    start = text.index("sunshine_register_component(NAME test_component_dualsense_usbip_gamepad ")
    return text[start:text.index("\nif(TARGET test_component_dualsense_usbip_gamepad)", start)]


def exported_dependencies(text):
    dependencies = set()
    for body in re.findall(r"target_link_libraries\((.*?)\)", text, re.S):
        scope = None
        for token in body.split():
            if token in {"PUBLIC", "INTERFACE", "PRIVATE"}:
                scope = token
            elif scope in {"PUBLIC", "INTERFACE"} and token.startswith("Boost::"):
                dependencies.add(token.removeprefix("Boost::"))
    return dependencies


class BoostComponentInterfaces(unittest.TestCase):
    def test_production_format_edge_is_declared(self):
        self.assertIn("#include <boost/format.hpp>", (ROOT / "src/stat_trackers.h").read_text())
        self.assertIn("Boost::format", registration().split("LINK_LIBRARIES", 1)[1])

    @unittest.skipUnless(os.environ.get("BOOST_MODULAR_ARCHIVE") and os.environ.get("CXX"),
                         "exact modular Boost archive and compiler were not supplied")
    def test_exact_modular_format_header_positive_and_missing_interface_negative(self):
        archive = Path(os.environ["BOOST_MODULAR_ARCHIVE"])
        with archive.open("rb") as archive_bytes:
            self.assertEqual(hashlib.file_digest(archive_bytes, "sha256").hexdigest(), ARCHIVE_SHA256)
        with tarfile.open(archive) as source, tempfile.TemporaryDirectory() as scratch:
            scratch = Path(scratch)
            members = source.getmembers()
            cmake = {}
            for member in members:
                match = re.fullmatch(r"[^/]+/libs/([^/]+)/CMakeLists.txt", member.name)
                if match:
                    cmake[match[1]] = source.extractfile(member).read().decode()
            closure = {"format"}
            pending = ["format"]
            while pending:
                for dependency in exported_dependencies(cmake[pending.pop()]):
                    if dependency not in closure:
                        self.assertIn(dependency, cmake)
                        closure.add(dependency)
                        pending.append(dependency)
            # Neither currently linked interface owns the production header.
            for module in ("log", "process"):
                self.assertNotIn("format", exported_dependencies(cmake[module]))
            for member in members:
                match = re.fullmatch(r"[^/]+/libs/([^/]+)/include/(.+)", member.name)
                if match and match[1] in closure and member.isfile():
                    relative = Path(match[2])
                    self.assertFalse(relative.is_absolute() or ".." in relative.parts)
                    target = scratch / match[1] / "include" / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(source.extractfile(member).read())
            poison = scratch / "fallback" / "boost" / "format.hpp"
            poison.parent.mkdir(parents=True)
            poison.write_text('#error "missing explicit Boost format interface"\n')
            probe = scratch / "probe.cpp"
            probe.write_text('#include <cstdint>\n#include "src/stat_trackers.h"\n'
                             'static_assert(sizeof(stat_trackers::min_max_avg_tracker<int>) > 0);\n')
            common = [os.environ["CXX"], "-std=c++17", "-fsyntax-only", "-I", str(ROOT)]
            include = {module: ["-I", str(scratch / module / "include")] for module in sorted(closure)}
            positive = subprocess.run(common + sum(include.values(), []) + ["-I", str(poison.parents[1]), str(probe)],
                                      capture_output=True, text=True)
            self.assertEqual(positive.returncode, 0, positive.stderr)
            negative = subprocess.run(common + sum((value for module, value in include.items() if module != "format"), [])
                                      + ["-I", str(poison.parents[1]), str(probe)], capture_output=True, text=True)
            self.assertNotEqual(negative.returncode, 0)
            self.assertIn("missing explicit Boost format interface", negative.stderr)


if __name__ == "__main__":
    unittest.main()
