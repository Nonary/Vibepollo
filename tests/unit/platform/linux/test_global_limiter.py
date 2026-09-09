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
print(json.dumps({k: v for k, v in g_session.env.items() if k.startswith(("DXVK", "VKD3D", "MANGOHUD", "KEEP_"))}))
''')
        self.original = b'user_settings = {"KEEP_SETTING": "yes", "DXVK_CONFIG": "dxvk.hud = fps"}\n'
        self.settings = self.tool / "user_settings.py"
        self.settings.write_bytes(self.original)
        self.settings.chmod(0o640)

    def server(self, provider="proton", millihz=59940):
        child = subprocess.Popen([sys.executable, "-I", str(self.source), provider, str(millihz), "custom", "0", "late", str(self.tool)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
        env = {k: v for k, v in os.environ.items() if not k.startswith(("DXVK", "VKD3D", "MANGOHUD", "LD_PRELOAD"))}
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

    def test_discovery_and_validation(self):
        self.assertEqual(limiter.tools_in([self.tool, self.tool]), {self.tool})
        for invalid in (["proton", 0, "custom", False, "late"], ["bogus", 60000, "custom", False, "late"], ["proton", 60000, "custom", False, "late", "extra"]):
            with self.assertRaises(ValueError):
                limiter.environment(invalid, {})


if __name__ == "__main__":
    unittest.main()
