"""Exercise real version/template output and the packaged MSI without installing it."""

import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[4]
UPGRADE_CODE = "{E3FA501A-85F8-4187-85A7-D6E6BDC7EDA1}"
GUARD = "Installed OR NOT WIX_UPGRADE_DETECTED"
NS = {"w": "http://schemas.microsoft.com/wix/2006/wi"}
MSI_PATH = None
BUILD_VERSION = None


def configure_contract(version, directory):
    """Include the whole production modules, not a copied version calculation."""
    directory = Path(directory)
    probe = directory / "contract.cmake"
    probe.write_text(r'''
cmake_minimum_required(VERSION 3.25)
set(CMAKE_SOURCE_DIR "${REPO_ROOT}")
include("${REPO_ROOT}/cmake/prep/build_version.cmake")
include("${REPO_ROOT}/cmake/packaging/windows_wix.cmake")
# Include the CPack serialization round trip used by the real package build.
set(CPACK_PACKAGE_NAME ValidationContract)
set(CPACK_OUTPUT_CONFIG_FILE "${OUTPUT_DIR}/CPackConfig.cmake")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "${OUTPUT_DIR}/CPackSourceConfig.cmake")
include(CPack)
unset(CPACK_WIX_VALIDATION_CONDITION)
include("${CPACK_OUTPUT_CONFIG_FILE}")
configure_file("${CPACK_WIX_TEMPLATE}" "${OUTPUT_DIR}/product.wxs" @ONLY)
file(WRITE "${OUTPUT_DIR}/contract.json"
  "{\"full\":\"${PROJECT_VERSION_FULL}\",\"numeric\":\"${PROJECT_VERSION_NUMERIC}\","
  "\"msi\":\"${CPACK_WIX_PRODUCT_VERSION}\",\"upgrade\":\"${CPACK_WIX_UPGRADE_GUID}\"}")
''', encoding="utf-8")
    environment = dict(os.environ)
    environment.pop("BUILD_VERSION", None)
    result = subprocess.run(
        ["cmake", f"-DREPO_ROOT={ROOT.as_posix()}", f"-DBUILD_VERSION={version}",
         f"-DOUTPUT_DIR={directory.as_posix()}", "-P", str(probe)],
        env=environment, capture_output=True, text=True,
    )
    if result.returncode:
        raise AssertionError(result.stdout + result.stderr)
    return json.loads((directory / "contract.json").read_text()), ET.parse(directory / "product.wxs")


class Msi:
    """Read MSI tables and evaluate conditions with Windows Installer itself."""

    def __init__(self, path):
        self.api = ctypes.WinDLL("msi")
        uint = ctypes.c_uint
        handle = ctypes.POINTER(uint)
        signatures = {
            "MsiOpenDatabaseW": [ctypes.c_wchar_p, ctypes.c_wchar_p, handle],
            "MsiDatabaseOpenViewW": [uint, ctypes.c_wchar_p, handle],
            "MsiViewExecute": [uint, uint],
            "MsiViewFetch": [uint, handle],
            "MsiRecordGetFieldCount": [uint],
            "MsiRecordGetStringW": [uint, uint, ctypes.c_wchar_p, handle],
            "MsiOpenPackageExW": [ctypes.c_wchar_p, uint, handle],
            "MsiSetPropertyW": [uint, ctypes.c_wchar_p, ctypes.c_wchar_p],
            "MsiEvaluateConditionW": [uint, ctypes.c_wchar_p],
            "MsiCloseHandle": [uint],
        }
        for name, arguments in signatures.items():
            function = getattr(self.api, name)
            function.argtypes = arguments
            function.restype = uint
        self.database = uint()
        self.session = uint()
        self.ole32 = ctypes.WinDLL("ole32")
        self.ole32.CoInitializeEx.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        self.ole32.CoInitializeEx.restype = ctypes.c_int32
        self.ole32.CoUninitialize.argtypes = []
        self.ole32.CoUninitialize.restype = None
        self.com_initialized = False
        try:
            # MsiOpenPackageEx requires COM initialized on this same thread.
            result = self.ole32.CoInitializeEx(None, 2)  # COINIT_APARTMENTTHREADED
            if result not in (0, 1):  # S_OK or S_FALSE, both need CoUninitialize.
                raise AssertionError(f"COM initialization failed: {result}")
            self.com_initialized = True
            self.check(self.api.MsiOpenDatabaseW(str(path), None, ctypes.byref(self.database)))
            # IGNOREMACHINESTATE: no install action, registry write, or elevation.
            self.check(self.api.MsiOpenPackageExW(str(path), 1, ctypes.byref(self.session)))
        except Exception:
            self.close()
            raise

    @staticmethod
    def check(result):
        if result:
            raise AssertionError(f"Windows Installer API failed: {result}")

    def close(self):
        for handle in (self.session, self.database):
            if handle.value:
                self.api.MsiCloseHandle(handle)
                handle.value = 0
        if self.com_initialized:
            self.ole32.CoUninitialize()
            self.com_initialized = False

    def query(self, sql):
        view = ctypes.c_uint()
        self.check(self.api.MsiDatabaseOpenViewW(self.database, sql, ctypes.byref(view)))
        rows = []
        try:
            self.check(self.api.MsiViewExecute(view, 0))
            while True:
                record = ctypes.c_uint()
                result = self.api.MsiViewFetch(view, ctypes.byref(record))
                if result == 259:  # ERROR_NO_MORE_ITEMS
                    return rows
                self.check(result)
                try:
                    row = []
                    for field in range(1, self.api.MsiRecordGetFieldCount(record) + 1):
                        value = ctypes.create_unicode_buffer(4096)
                        size = ctypes.c_uint(len(value))
                        self.check(self.api.MsiRecordGetStringW(record, field, value, ctypes.byref(size)))
                        row.append(value.value)
                    rows.append(row)
                finally:
                    self.api.MsiCloseHandle(record)
        finally:
            self.api.MsiCloseHandle(view)

    def evaluate(self, condition, **properties):
        for name, value in properties.items():
            self.check(self.api.MsiSetPropertyW(self.session, name, value))
        result = self.api.MsiEvaluateConditionW(self.session, condition)
        if result not in (0, 1):
            raise AssertionError(f"Invalid MSI condition {condition!r}: {result}")
        return bool(result)


class ValidationContractTest(unittest.TestCase):
    def test_zero_version_is_still_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(AssertionError, "Version resolution produced 0.0.0"):
                configure_contract("0.0.0", directory)

    def test_configured_validation_and_production_contracts(self):
        for version, numeric, msi_version, validation in (
            ("2.0.0-pr-validation", "2.0.0", "2.0.90.0", True),
            ("v2.0.0-pr-validation", "2.0.0", "2.0.90.0", True),
            ("2.0.2-daily.d20261022+aaaaaaaaaaaa", "2.0.2", "2.0.290.0", False),
            ("2.0.0", "2.0.0", "2.0.99.0", False),
            ("2.0.2-stable.1", "2.0.2", "2.0.299.0", False),
            ("2.0.2-beta.1", "2.0.2", "2.0.231.0", False),
        ):
            with self.subTest(version=version), tempfile.TemporaryDirectory() as directory:
                contract, template = configure_contract(version, directory)
                self.assertEqual(contract["full"], version.removeprefix("v"))
                self.assertEqual(contract["numeric"], numeric)
                self.assertEqual(contract["msi"], msi_version)
                self.assertEqual(contract["upgrade"], UPGRADE_CODE)
                conditions = [element.text for element in template.findall("w:Product/w:Condition", NS)]
                self.assertEqual(conditions, [GUARD] if validation else [])
                upgrade = template.find("w:Product/w:MajorUpgrade", NS)
                self.assertEqual(upgrade.attrib, {"Schedule": "afterInstallInitialize", "AllowDowngrades": "yes"})
                marker = template.find("w:Product/w:Property[@Id='VIBESHINE_TRANSACTIONAL_REPLACEMENT']", NS)
                self.assertEqual(marker.get("Value"), "1")

    @unittest.skipUnless(os.name == "nt", "compiled MSI inspection requires Windows Installer")
    def test_actual_packaged_msi(self):
        if MSI_PATH is None:
            self.skipTest("pass --msi and --version after packaging to inspect the actual artifact")
        with tempfile.TemporaryDirectory() as directory:
            contract, template = configure_contract(BUILD_VERSION, directory)
        expected_guards = [element.text for element in template.findall("w:Product/w:Condition", NS)]
        msi = Msi(MSI_PATH)
        try:
            properties = dict(msi.query("SELECT `Property`, `Value` FROM `Property`"))
            self.assertEqual(properties["ProductVersion"], contract["msi"])
            self.assertEqual(properties["UpgradeCode"].upper(), UPGRADE_CODE)
            self.assertEqual(properties["VIBESHINE_TRANSACTIONAL_REPLACEMENT"], "1")
            # WiX AllowDowngrades emits an inclusive zero lower bound and no
            # upper bound, detecting older, equal, and newer family members.
            rows = msi.query(
                "SELECT `UpgradeCode`, `VersionMin`, `VersionMax`, `ActionProperty`, `Attributes` FROM `Upgrade`"
            )
            upgrade_rows = [row for row in rows if row[:4] == [UPGRADE_CODE, "0", "", "WIX_UPGRADE_DETECTED"]]
            self.assertEqual(len(upgrade_rows), 1, rows)
            self.assertTrue(int(upgrade_rows[0][4]) & 0x100)  # VersionMinInclusive
            tables = {row[0] for row in msi.query("SELECT `Name` FROM `_Tables`")}
            conditions = (
                [row[0] for row in msi.query("SELECT `Condition` FROM `LaunchCondition`")]
                if "LaunchCondition" in tables else []
            )
            self.assertEqual([condition for condition in conditions if condition == GUARD], expected_guards)
            if expected_guards:
                for table in ("InstallUISequence", "InstallExecuteSequence"):
                    sequence = {
                        name: int(order)
                        for name, order in msi.query(f"SELECT `Action`, `Sequence` FROM `{table}`")
                    }
                    self.assertLess(sequence["FindRelatedProducts"], sequence["LaunchConditions"])
                    if table == "InstallExecuteSequence":
                        self.assertLess(sequence["LaunchConditions"], sequence["InstallInitialize"])
                        self.assertLess(sequence["LaunchConditions"], sequence["RemoveExistingProducts"])
                # Evaluate the condition read back from the compiled MSI, not a
                # Python imitation, in both UI and quiet-install property states.
                related_product = "{11111111-1111-1111-1111-111111111111}"
                cases = (("", "", True), ("", related_product, False), ("1", "", True), ("1", related_product, True))
                for installed, related, allowed in cases:
                    for ui_level in ("2", "5"):
                        with self.subTest(installed=installed, related=related, ui=ui_level):
                            self.assertEqual(
                                msi.evaluate(expected_guards[0], Installed=installed,
                                             WIX_UPGRADE_DETECTED=related, UILevel=ui_level),
                                allowed,
                            )
        finally:
            msi.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--msi", type=Path)
    parser.add_argument("--version")
    args, remaining = parser.parse_known_args()
    if bool(args.msi) != bool(args.version):
        parser.error("--msi and --version must be supplied together")
    MSI_PATH, BUILD_VERSION = args.msi.resolve() if args.msi else None, args.version
    unittest.main(argv=[__file__, *remaining])
