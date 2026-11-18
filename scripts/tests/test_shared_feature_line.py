import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


def load_module(name):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).resolve().parents[1] / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


shared = load_module("shared_feature_line")
numeric = load_module("numeric_release")
SHINE, POLLO = shared.BASELINES
CANARY = "refs/heads/canary"
MASTER = "refs/heads/master"


class SharedFeatureHistoryTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.repo = temp.name
        self.git("init", "-q")
        self.tree = self.git("mktree")
        self.root = self.commit("chore: root")
        self.baseline_feature = self.commit("feat: already counted", self.root)
        self.baselines = {
            SHINE: {CANARY: self.baseline_feature, MASTER: self.root},
            POLLO: {CANARY: self.root, MASTER: self.root},
        }
        for name, value in (("git", self.git), ("BASELINES", self.baselines),
                            ("HISTORICAL_TAGS", {SHINE: {}, POLLO: {}})):
            patcher = patch.object(shared, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def git(self, *args, stdin=None):
        return subprocess.run(
            ["git", "-C", self.repo, "-c", "user.name=Release Test",
             "-c", "user.email=test@example.invalid", *args],
            input=stdin, stdin=None if stdin is not None else subprocess.DEVNULL,
            check=True, capture_output=True, text=True).stdout

    def commit(self, message, parent=None):
        parents = ["-p", parent] if parent else []
        return self.git("commit-tree", self.tree.strip(), *parents, stdin=message).strip()

    def test_fix_late_port_real_feature_and_port_back(self):
        shine_fix = self.commit("fix: Shine only", self.baseline_feature)
        old_port = self.commit(f"feat: adapted older feature\n\n"
                               f"(cherry picked from commit {self.baseline_feature})", self.root)
        self.assertEqual(shared.feature_line({
            SHINE: {CANARY: shine_fix, MASTER: self.root},
            POLLO: {CANARY: old_port, MASTER: self.root},
        }), (2, 18))

        pollo_feature = self.commit("feat: Pollo first", old_port)
        tips = {SHINE: {CANARY: shine_fix, MASTER: self.root},
                POLLO: {CANARY: old_port, MASTER: pollo_feature}}
        self.assertEqual(shared.feature_line(tips), (2, 19))
        # A subsequent port in either direction preserves the first origin.
        new_port = self.commit(f"feat: adapted Pollo feature\n\n"
                               f"(cherry picked from commit {pollo_feature})", shine_fix)
        port_back = self.commit(f"feat: adapted again\n\n"
                                f"(cherry picked from commit {pollo_feature})\n"
                                f"(cherry picked from commit {new_port})", old_port)
        tips[SHINE][CANARY] = new_port
        tips[POLLO][CANARY] = port_back
        self.assertEqual(shared.feature_line(tips), (2, 19))

    def test_simultaneous_distinct_features_and_shared_change_id(self):
        change_id = "I" + "a" * 40
        shine_feature = self.commit(f"feat: Shine\n\nChange-Id: {change_id}", self.baseline_feature)
        pollo_feature = self.commit(f"feat: Pollo port\n\nChange-Id: {change_id}", self.root)
        independent = self.commit("feat: independent", pollo_feature)
        tips = {SHINE: {CANARY: shine_feature, MASTER: self.root},
                POLLO: {CANARY: independent, MASTER: self.root}}
        self.assertEqual(shared.feature_line(tips), (2, 20))

    def test_peer_ref_read_failure_never_falls_back_to_local_release(self):
        self.git("update-ref", CANARY, self.baseline_feature)
        self.git("update-ref", MASTER, self.root)
        with patch.object(shared, "public_url", return_value=self.repo):
            with self.assertRaisesRegex(ValueError, "Missing protected source or bootstrap tag"):
                shared.read_refs(SHINE, numeric.supported_version_key)

    def test_incoherent_snapshot_and_higher_reservation_fail_closed(self):
        snapshots = {
            SHINE: {**self.baselines[SHINE], "refs/tags/2.14.8": self.baseline_feature},
            POLLO: {**self.baselines[POLLO], "refs/tags/2.17.6": self.root},
        }
        local = {"2.14.8": self.baseline_feature}
        with patch.object(shared, "load_tips", side_effect=lambda repo, refs: {
                ref: refs[ref] for ref in self.baselines[repo]}):
            with patch.object(shared, "read_refs", side_effect=[
                snapshots[SHINE], snapshots[POLLO], snapshots[POLLO], snapshots[SHINE],
            ]):
                self.assertEqual(shared.current_line(SHINE, "canary", self.baseline_feature,
                    local, numeric.supported_version_key, require_tip=True), (2, 18))

            changed_peer = {**snapshots[POLLO], CANARY: self.commit("feat: race", self.root)}
            with patch.object(shared, "read_refs", side_effect=[
                snapshots[SHINE], snapshots[POLLO], changed_peer,
            ]):
                with self.assertRaisesRegex(ValueError, "changed while reading"):
                    shared.current_line(SHINE, "canary", self.baseline_feature,
                        local, numeric.supported_version_key, require_tip=True)

            ahead_peer = {**snapshots[POLLO], "refs/tags/2.19.0": self.root}
            with patch.object(shared, "read_refs", side_effect=[snapshots[SHINE], ahead_peer]):
                with self.assertRaisesRegex(ValueError, "behind an existing numeric reservation"):
                    shared.current_line(SHINE, "canary", self.baseline_feature,
                        local, numeric.supported_version_key, require_tip=True)

            with patch.object(shared, "read_refs", side_effect=[snapshots[SHINE], snapshots[POLLO]]):
                with self.assertRaisesRegex(ValueError, "Local numeric tag state changed"):
                    shared.current_line(SHINE, "canary", self.baseline_feature,
                        {}, numeric.supported_version_key, require_tip=True)

            with patch.object(shared, "read_refs", side_effect=[snapshots[SHINE], snapshots[POLLO]]):
                with self.assertRaisesRegex(ValueError, "Release source advanced"):
                    shared.current_line(SHINE, "canary", self.root,
                        local, numeric.supported_version_key, require_tip=True)


if __name__ == "__main__":
    unittest.main()
