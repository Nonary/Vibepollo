from concurrent.futures import ThreadPoolExecutor
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
daily = load_module("daily_release")
SHINE, POLLO = shared.BASELINES
CANARY = "refs/heads/canary"
MASTER = "refs/heads/master"
LOCAL = "Nonary/Vibepollo"


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

    def test_simultaneous_allocations_and_frozen_validation_after_peer_release(self):
        peer = POLLO if LOCAL == SHINE else SHINE

        def published(tag, *, prerelease=False):
            return dict(tag_name=tag, draft=False, prerelease=prerelease,
                        published_at="2026-11-18T12:00:00Z")

        for branch, channel in (("canary", "nightly"), ("master", "stable")):
            with self.subTest(channel=channel):
                ref = f"refs/heads/{branch}"
                local_base = self.baselines[LOCAL][ref]
                peer_base = self.baselines[peer][ref]
                local_source = self.commit("fix: local", local_base)
                peer_source = self.commit("fix: second peer fix",
                                          self.commit("fix: first peer fix", peer_base))
                inputs = {LOCAL: {**self.baselines[LOCAL], ref: local_source,
                                  "refs/tags/2.17.5": local_base},
                          peer: {**self.baselines[peer], ref: peer_source,
                                 "refs/tags/2.17.6": peer_base}}
                local_pages = [[published("2.17.5")]]
                peer_pages = [[published("2.17.6")]]

                def git_command(*args):
                    self.assertEqual(args[0], "git")
                    return self.git(*args[1:]).strip()

                def allocate(repo, pages, source):
                    refs = {key.removeprefix("refs/tags/"): oid
                            for key, oid in inputs[repo].items() if key.startswith("refs/tags/")}
                    line = shared.current_line(repo, branch, source, refs,
                                               numeric.supported_version_key, require_tip=True)
                    return numeric.resolve(pages, refs, source, branch, channel, feature_line=line)

                with patch.object(shared, "load_tips", side_effect=lambda repo, refs: {
                        key: refs[key] for key in self.baselines[repo]}), \
                     patch.object(shared, "read_refs", side_effect=lambda repo, _: inputs[repo].copy()), \
                     patch.object(numeric, "run", side_effect=git_command):
                    with ThreadPoolExecutor(max_workers=2) as executor:
                        local_result = executor.submit(allocate, LOCAL, local_pages, local_source)
                        peer_result = executor.submit(allocate, peer, peer_pages, peer_source)
                        self.assertEqual(local_result.result()["tag"], "2.18.1")
                        self.assertEqual(peer_result.result()["tag"], "2.18.2")

                    # Both reservations exist now. The peer publishes first,
                    # then lands and publishes a feature before we finish.
                    tag = "2.18.1"
                    inputs[LOCAL][f"refs/tags/{tag}"] = local_source
                    inputs[peer]["refs/tags/2.18.2"] = peer_source
                    peer_pages[0].append(published("2.18.2", prerelease=channel == "nightly"))
                    feature = self.commit("feat: peer feature", peer_source)
                    inputs[peer][ref] = feature
                    self.assertEqual(allocate(peer, peer_pages, feature)["tag"], "2.19.0")
                    inputs[peer]["refs/tags/2.19.0"] = feature
                    peer_pages[0].append(published("2.19.0", prerelease=channel == "nightly"))
                    port = self.commit(f"feat: late port\n\n(cherry picked from commit {feature})",
                                       local_source)
                    inputs[LOCAL][ref] = port
                    local_refs = {"2.17.5": local_base, tag: local_source}
                    self.assertEqual(shared.current_line(LOCAL, branch, port, local_refs,
                        numeric.supported_version_key, require_tip=True), (2, 19))

                    def stable_git(*args):
                        if args[:2] == ("git", "fetch"):
                            return ""
                        if args == ("git", "rev-parse", "refs/remotes/origin/numeric-release-source^{commit}"):
                            return port
                        return git_command(*args)

                    with patch.dict("os.environ", GITHUB_REPOSITORY=LOCAL,
                                    DAILY_RELEASE_SOURCE_BRANCH=branch,
                                    DAILY_RELEASE_SOURCE_CONFIRMED=f"{LOCAL}:{branch}",
                                    RELEASE_SOURCE_BRANCH=branch, RELEASE_PRIMARY_BRANCH=branch,
                                    RELEASE_SOURCE_CONFIRMED=f"{LOCAL}:{branch}",
                                    RELEASE_CHANNEL=channel, RELEASE_SOURCE_COMMIT=local_source,
                                    RELEASE_TAG=tag, GITHUB_OUTPUT=str(Path(self.repo) / "output")), \
                         patch.object(daily, "run", side_effect=["", port, ""] * 2), \
                         patch.object(daily, "tag_refs", return_value=(local_refs, {tag: channel})), \
                         patch.object(daily, "previous_successful_nightly", return_value=None), \
                         patch.object(daily.numeric_release, "release_pages", return_value=local_pages) as releases, \
                         patch.object(daily.numeric_release, "run", side_effect=git_command), \
                         patch.object(daily.numeric_release, "live_feature_line",
                                      side_effect=AssertionError("Validation queried peer changes")), \
                         patch.object(numeric, "live_feature_line",
                                      side_effect=AssertionError("Validation queried peer changes")):
                        def validate():
                            if channel == "stable":
                                with patch.object(numeric, "run", side_effect=stable_git), \
                                     patch.object(numeric, "read_refs", return_value=(local_refs, {tag: channel})), \
                                     patch.object(numeric, "release_pages", return_value=local_pages) as local_releases, \
                                     patch.object(numeric, "latest_qualified_nightly", return_value=None):
                                    result = numeric.validate_tag_from_environment()
                                    local_releases.assert_called_with(LOCAL)
                                    return result["tag"]
                            daily.validate_live_numeric_version(LOCAL, tag, local_source)
                            releases.assert_called_with(LOCAL)
                            return tag

                        self.assertEqual(validate(), tag)
                        local_refs["2.19.1"] = port
                        local_pages[0].append(published("2.19.1"))
                        with self.assertRaisesRegex(ValueError, "outrank all published releases"):
                            validate()

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
