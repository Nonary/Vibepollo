"""Exercise the real launch hook in a separate Proton-style process."""
import importlib.util
import json
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import unittest
import uuid

SOURCE = Path(__file__).resolve().parents[4] / "packaging/linux/vibepollo-global-limiter.py"
spec = importlib.util.spec_from_file_location("limiter", SOURCE)
limiter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(limiter)


class GlobalLimiter(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.tool = Path(self.temporary.name)
        self.source = self.tool / "helper.py"
        self.source.write_bytes(SOURCE.read_bytes().replace(b"vibepollo.proton-limiter.v1.", ("vibepollo.test." + uuid.uuid4().hex + ".").encode()))
        (self.tool / "user_settings.sample.py").write_text("user_settings = {}\n")
        # Match Proton's copy-before-import and missing-keys-only semantics.
        (self.tool / "proton").write_text('''import os, json
class Session: pass
g_session = Session()
g_session.env = dict(os.environ)
import user_settings
for key, value in user_settings.user_settings.items():
    g_session.env.setdefault(key, value)
print(json.dumps({k: v for k, v in g_session.env.items() if k.startswith(("DXVK", "VKD3D", "MANGOHUD", "PROTON_", "ENABLE_HDR_WSI", "KEEP_"))}))
''')
        self.original = b'user_settings = {"KEEP_SETTING": "yes", "DXVK_CONFIG": "dxvk.hud = fps"}\n'
        self.settings = self.tool / "user_settings.py"
        self.settings.write_bytes(self.original)
        self.settings.chmod(0o640)

    def server(self, provider="proton", millihz=59940, color_mode="sdr", wayland_hdr_compatibility=False, dualsense=True):
        child = subprocess.Popen([sys.executable, "-I", str(self.source), provider, str(millihz), "custom", "0", "late", color_mode, "1" if wayland_hdr_compatibility else "0", "1" if dualsense else "0", str(self.tool)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        def stop():
            if child.poll() is None:
                child.terminate()
                child.wait(timeout=8)
            child.stdout.close()
            child.stderr.close()
        self.addCleanup(stop)
        self.assertTrue(select.select([child.stdout], [], [], 8)[0], "helper readiness timed out")
        ready = child.stdout.readline()
        self.assertEqual(ready, "READY 1\n", child.stderr.read() if not ready else ready)
        return child

    def launch(self, **overrides):
        env = {k: v for k, v in os.environ.items() if not k.startswith(("DXVK", "VKD3D", "MANGOHUD", "PROTON_", "ENABLE_HDR_WSI", "LD_PRELOAD"))}
        env.update(overrides)
        return json.loads(subprocess.check_output([sys.executable, str(self.tool / "proton")], env=env, text=True))

    def test_external_launch_and_disconnect(self):
        child = self.server()
        active = self.launch(DXVK_FRAME_RATE="30", VKD3D_FRAME_RATE="30", KEEP_ENV="untouched", MANGOHUD="1")
        self.assertEqual(active["VKD3D_FRAME_RATE"], "59.94")
        self.assertEqual(active["DXVK_FRAME_RATE"], "60")
        self.assertIn("dxvk.hud = fps", active["DXVK_CONFIG"])
        self.assertIn("dxgi.maxFrameRate = 60", active["DXVK_CONFIG"])
        self.assertNotIn("MANGOHUD", active)
        self.assertEqual(active["PROTON_ENABLE_HDR"], "0")
        self.assertEqual(active["DXVK_HDR"], "0")
        self.assertEqual(active["KEEP_ENV"], "untouched")
        self.assertEqual(active["KEEP_SETTING"], "yes")
        managed = self.launch(VIBEPOLLO_LIMITER_MANAGED="1", VKD3D_FRAME_RATE="45")
        self.assertEqual(managed["VKD3D_FRAME_RATE"], "45")
        self.assertEqual(self.settings.read_bytes(), self.original + limiter.BLOCK)
        self.assertEqual(self.settings.stat().st_mode & 0o777, 0o640)
        child.terminate()
        child.wait(timeout=8)
        inactive = self.launch(DXVK_FRAME_RATE="30")
        self.assertEqual(inactive["DXVK_FRAME_RATE"], "30")
        self.assertNotIn("VKD3D_FRAME_RATE", inactive)
        self.assertEqual(inactive["DXVK_CONFIG"], "dxvk.hud = fps")

    def test_stream_color_modes_without_limiter(self):
        child = self.server("disabled", 0, "hdr")
        hdr = self.launch()
        self.assertEqual(hdr["PROTON_ENABLE_HDR"], "1")
        self.assertEqual(hdr["DXVK_HDR"], "1")
        self.assertNotIn("ENABLE_HDR_WSI", hdr)
        self.assertNotIn("PROTON_ENABLE_WAYLAND", hdr)
        explicit = self.launch(PROTON_ENABLE_HDR="0", DXVK_HDR="0")
        self.assertEqual(explicit["PROTON_ENABLE_HDR"], "0")
        self.assertEqual(explicit["DXVK_HDR"], "0")
        child.terminate()
        child.wait(timeout=8)

        self.server("disabled", 0, "sdr10")
        sdr10 = self.launch()
        self.assertEqual(sdr10["PROTON_ENABLE_HDR"], "0")
        self.assertEqual(sdr10["DXVK_HDR"], "0")

    def test_wayland_hdr_compatibility_is_hdr_only_and_preserves_explicit_values(self):
        child = self.server("disabled", 0, "hdr", True)
        active = self.launch()
        self.assertEqual(active["ENABLE_HDR_WSI"], "1")
        self.assertEqual(active["PROTON_ENABLE_WAYLAND"], "1")
        self.assertEqual(active["PROTON_ENABLE_HDR"], "1")
        self.assertEqual(active["DXVK_HDR"], "1")
        explicit = self.launch(
            ENABLE_HDR_WSI="0",
            PROTON_ENABLE_WAYLAND="0",
            PROTON_ENABLE_HDR="0",
            DXVK_HDR="0",
        )
        self.assertEqual(explicit["ENABLE_HDR_WSI"], "0")
        self.assertEqual(explicit["PROTON_ENABLE_WAYLAND"], "0")
        self.assertEqual(explicit["PROTON_ENABLE_HDR"], "0")
        self.assertEqual(explicit["DXVK_HDR"], "0")
        child.terminate()
        child.wait(timeout=8)

    def test_managed_limiter_still_receives_wayland_hdr_flags(self):
        child = self.server("mangohud-proton", 120000, "hdr", True)
        managed = self.launch(
            VIBEPOLLO_LIMITER_MANAGED="1",
            VKD3D_FRAME_RATE="120",
            MANGOHUD="1",
            MANGOHUD_CONFIG="read_cfg,fps_limit=0",
        )
        self.assertEqual(managed["ENABLE_HDR_WSI"], "1")
        self.assertEqual(managed["PROTON_ENABLE_WAYLAND"], "1")
        self.assertEqual(managed["PROTON_ENABLE_HDR"], "1")
        self.assertEqual(managed["DXVK_HDR"], "1")
        self.assertEqual(managed["VKD3D_FRAME_RATE"], "120")
        self.assertEqual(managed["MANGOHUD_CONFIG"], "read_cfg,fps_limit=0")
        child.terminate()
        child.wait(timeout=8)

        self.server("disabled", 0, "sdr")
        sdr = self.launch()
        self.assertNotIn("ENABLE_HDR_WSI", sdr)
        self.assertNotIn("PROTON_ENABLE_WAYLAND", sdr)

    def test_dualsense_is_global_independent_of_limiter_and_hdr(self):
        child = self.server("disabled", 0, "sdr")
        for app_id in ("3768760", "1182900"):
            for managed in ("0", "1"):
                active = self.launch(SteamAppId=app_id, VIBEPOLLO_LIMITER_MANAGED=managed)
                self.assertEqual(active["PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE"], "1")
                self.assertEqual(active["PROTON_SONY_WINDOWS_DEVICE_NAMES"], "1")
                self.assertNotIn("VKD3D_FRAME_RATE", active)
        explicit = self.launch(PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE="0",
                               PROTON_SONY_WINDOWS_DEVICE_NAMES="0")
        self.assertEqual(explicit["PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE"], "0")
        self.assertEqual(explicit["PROTON_SONY_WINDOWS_DEVICE_NAMES"], "0")
        child.kill()
        child.wait(timeout=8)
        inactive = self.launch()
        self.assertNotIn("PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE", inactive)
        self.assertNotIn("PROTON_SONY_WINDOWS_DEVICE_NAMES", inactive)
        self.server("disabled", 0, "sdr", dualsense=False)
        disabled = self.launch()
        self.assertNotIn("PROTON_KEEP_SONY_AUDIO_ENDPOINT_VISIBLE", disabled)
        self.assertNotIn("PROTON_SONY_WINDOWS_DEVICE_NAMES", disabled)

    def test_crash_leaves_inert_hook(self):
        child = self.server()
        child.kill()
        child.wait(timeout=8)
        self.assertNotIn("VKD3D_FRAME_RATE", self.launch())
        limiter.install(self.tool, self.source.read_bytes())
        self.assertEqual(self.settings.read_bytes(), self.original + limiter.BLOCK)

    def test_fractional_mangohud_and_combined(self):
        child = self.server("mangohud")
        active = self.launch(MANGOHUD_FPS_LIMIT="30", MANGOHUD_CONFIG="fps_limit=30")
        self.assertNotIn("MANGOHUD_FPS_LIMIT", active)
        self.assertTrue(active["MANGOHUD_CONFIG"].endswith("fps_limit=59.94"))
        child.terminate()
        child.wait(timeout=8)
        self.server("mangohud-proton")
        combined = self.launch(MANGOHUD_FPS_LIMIT="30")
        self.assertEqual(combined["MANGOHUD"], "1")
        self.assertTrue(combined["MANGOHUD_CONFIG"].endswith("fps_limit=0"))
        self.assertEqual(combined["VKD3D_FRAME_RATE"], "59.94")

    def test_no_settings_and_symlink_preservation(self):
        self.settings.unlink()
        limiter.install(self.tool, self.source.read_bytes())
        self.assertEqual(self.settings.read_bytes(), limiter.BLOCK)
        self.settings.unlink()
        other = self.tool / "other.py"
        other.write_bytes(self.original)
        self.settings.symlink_to(other)
        with self.assertRaises(OSError):
            limiter.install(self.tool, self.source.read_bytes())
        self.assertEqual(other.read_bytes(), self.original)

    def test_old_hook_is_migrated(self):
        self.settings.write_bytes(self.original + limiter.OLD_BLOCK)
        limiter.install(self.tool, self.source.read_bytes())
        self.assertEqual(self.settings.read_bytes(), self.original + limiter.BLOCK)

    def test_discovery_and_validation(self):
        self.assertEqual(limiter.tools_in([self.tool, self.tool]), {self.tool})
        for invalid in (
            ["proton", 0, "custom", False, "late", "sdr", False, False],
            ["bogus", 60000, "custom", False, "late", "sdr", False, False],
            ["proton", 60000, "custom", False, "late", "invalid", False, False],
            ["disabled", 60000, "custom", False, "late", "hdr", False, False],
            ["disabled", 0, "1", False, "late", "hdr", False, False],
            ["proton", 60000, "custom", False, "late", "sdr", True, False],
            ["proton", 60000, "custom", False, "late", "sdr", False, "extra"],
        ):
            with self.assertRaises(ValueError):
                limiter.environment(invalid, {})


if __name__ == "__main__":
    unittest.main()
