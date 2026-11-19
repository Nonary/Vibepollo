"""Test Arch bridge activation, including stale compositor-wrapper aliases."""

from pathlib import Path
import runpy
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ARCH = Path(__file__).resolve().parents[4] / "packaging/linux/Arch"
HELPER = runpy.run_path(str(ARCH / "vibepollo-kwin-bridge-rebuild"))["activate"].__globals__
PRODUCT = HELPER["PRODUCT"]


class BridgeRefreshTest(unittest.TestCase):
    def setUp(self):
        self.sandbox = tempfile.TemporaryDirectory()
        self.addCleanup(self.sandbox.cleanup)
        self.libdir = Path(self.sandbox.name)
        self.kwin = self.libdir / "libkwin.so.6.8.2"
        (self.libdir / "libkwin.so").symlink_to(self.kwin)
        self.kwin.write_bytes(b"KWin")
        package = self.libdir / PRODUCT / "kwin-bridge"
        package.mkdir(parents=True)
        self.candidate = package / HELPER["BASE"]
        self.candidate.write_bytes(b"package-provided bridge")
        self.candidate.chmod(0o644)
        self.header = package / "kwin_bridge_abi.h"
        self.header.write_text("ABI header")
        self.aliases = [self.libdir / name for name in HELPER["ALIASES"]]
        self.activation = self.libdir / HELPER["ACTIVATION"]

    def establish_old_preloads(self):
        for alias in self.aliases:
            alias.write_bytes(b"older bridge inode")
        self.activation.write_text(f"LD_PRELOAD={self.aliases[1].name}\n")

    def assert_inactive(self):
        self.assertEqual(self.activation.read_text(), "LD_PRELOAD=\n")
        for alias in self.aliases:
            self.assertFalse(alias.exists())
        self.assertEqual(self.candidate.read_bytes(), b"package-provided bridge")
        self.assertEqual(stat.S_IMODE(self.candidate.stat().st_mode), 0o644)

    def test_deactivate_handles_old_wrappers(self):
        self.establish_old_preloads()
        with self.aliases[1].open("rb") as mapped:
            HELPER["deactivate"](self.libdir)
            self.assertEqual(mapped.read(), b"older bridge inode")
        self.assert_inactive()

    def test_compatible_bridge_uses_only_verified_package_bytes(self):
        self.establish_old_preloads()
        with patch.dict(HELPER, trusted_path=lambda *args: None,
                        verify_native_abi=lambda kwin, header, *_: self.assertEqual((kwin, header), (self.kwin, self.header)),
                        exports=lambda *_: HELPER["BRIDGE_EXPORTS"]):
            HELPER["activate"](self.libdir, self.candidate, self.header, self.kwin, None)
        for alias in self.aliases:
            self.assertEqual(alias.read_bytes(), self.candidate.read_bytes())
            self.assertEqual(stat.S_IMODE(alias.stat().st_mode), 0o4755)
        self.assertEqual(self.activation.read_text(), f"LD_PRELOAD={self.aliases[0].name}\n")

    def test_missing_kwin_is_inactive(self):
        self.establish_old_preloads()
        self.kwin.unlink()
        with patch.dict(HELPER, LIBDIR=self.libdir, trusted_path=lambda *_, **__: None):
            HELPER["update_installed_bridge"]()
        self.assert_inactive()

    def test_signature_mismatch_clears_both_aliases(self):
        self.establish_old_preloads()
        def bad_signature(*_):
            raise ValueError("installed signature is incompatible")
        with patch.dict(HELPER, LIBDIR=self.libdir, trusted_path=lambda *_, **__: None,
                        pwd=SimpleNamespace(getpwnam=lambda _: SimpleNamespace(pw_uid=100, pw_gid=100)),
                        verify_native_abi=bad_signature), patch.object(HELPER["os"], "chown"):
            with self.assertRaisesRegex(ValueError, "signature is incompatible"):
                HELPER["update_installed_bridge"]()
        self.assert_inactive()

    def test_same_soname_replacement_stays_inactive(self):
        self.establish_old_preloads()
        def replace_kwin(*_):
            staged = self.kwin.with_suffix(".new")
            staged.write_bytes(b"replacement")
            staged.replace(self.kwin)
        with patch.dict(HELPER, LIBDIR=self.libdir, trusted_path=lambda *_, **__: None,
                        pwd=SimpleNamespace(getpwnam=lambda _: SimpleNamespace(pw_uid=100, pw_gid=100)),
                        verify_native_abi=replace_kwin,
                        exports=lambda *_: HELPER["BRIDGE_EXPORTS"]), patch.object(HELPER["os"], "chown"):
            with self.assertRaisesRegex(ValueError, "changed during bridge verification"):
                HELPER["update_installed_bridge"]()
        self.assert_inactive()

    def test_legacy_bridge_keeps_supported_allocator(self):
        with patch.dict(HELPER, exports=lambda *_: {HELPER["LEGACY_ALLOCATOR"]}):
            HELPER["verify_native_abi"](self.kwin, self.header, self.libdir, None)
        with patch.dict(HELPER, exports=lambda *_: {HELPER["NATIVE_ALLOCATOR"]}):
            with self.assertRaisesRegex(ValueError, "allocator interface is unsupported"):
                HELPER["verify_native_abi"](self.kwin, self.header, self.libdir, None)

    def test_package_wires_transaction_paths_and_dropin_precedence(self):
        pre = (ARCH / f"00-{PRODUCT}-kwin-bridge-deactivate.hook").read_text()
        post = (ARCH / f"95-{PRODUCT}-kwin-bridge.hook").read_text()
        candidate = f"usr/lib/{PRODUCT}/kwin-bridge/{HELPER['BASE']}"
        helper = f"/usr/libexec/vibeshine/{PRODUCT}-kwin-bridge-rebuild"
        for hook in (pre, post):
            self.assertIn("Target = usr/lib/libkwin.so*\n", hook)
            self.assertIn(f"Target = {candidate}\n", hook)
            self.assertIn(f"Depends = {PRODUCT}\n", hook)
        self.assertIn(f"Exec = {helper} --deactivate\nAbortOnFail\n", pre)
        self.assertIn(f"Exec = {helper}\n", post)
        self.assertIn("Operation = Remove\n", pre)
        self.assertIn("When = PreTransaction\n", pre)
        self.assertIn("When = PostTransaction\n", post)
        dropin = ARCH / "vibeshine-kwin-local-gpu.conf"
        self.assertGreater(dropin.name, "vibeshine-kwin-gpu.conf")
        self.assertIn(f"Environment=LD_PRELOAD=\nEnvironmentFile=-/usr/lib/{HELPER['ACTIVATION']}\n",
                      dropin.read_text())


if __name__ == "__main__":
    unittest.main()
