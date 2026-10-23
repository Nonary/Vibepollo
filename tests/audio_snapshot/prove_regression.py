#!/usr/bin/env python3
"""Build complete production components and replay pre-fix behavior on OS fakes.

The new seams do not exist at historical HEAD. Baseline patches preserve the
refactoring and restore each historical bug independently. Release-order mutations
also test the transport hold contract. None is an original native-Windows build
or source extraction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repository", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--compiler", default="/usr/bin/g++-15")
args = parser.parse_args()
repository = args.repository.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=False)
files = [
    "src/app_audio_termination.h", "src/audio_context.cpp", "src/audio_context.h", "src/audio_role_control.h",
    "src/platform/audio.h", "src/audio_lifecycle_state.h", "src/audio_lifecycle_policy.h",
    "src/thread_safe.h", "src/utility.h", "tests/unit/test_audio_context.cpp",
    "src/webrtc_capture_owner.h",
    "src/audio_display_transition.h", "src/config_display_transition.h",
    "tests/audio_snapshot/CMakeLists.txt",
]
manifest = {"native_target_base": "7a5dc658977b717da615a1067c1dc94578b61347",
            "native_target_staged_sha256": "b723b89ffd185f58acbcf03e6e98c888414dcc5f5a3a26da85e5f40b0fa9c59b",
            "historical_base": "69e163f33179860b8c63e6dd36ecc73f144e6fbd",
            "scope": "Complete portable production components with device/process/thread boundaries; no native Windows claim.",
            "baseline_scope": "Refactor-equivalent historical bugs and v2 owner failures plus release-order regression mutations; not unrefactored historical translation units.",
            "inputs": {}, "runs": []}
for name in files:
    manifest["inputs"][name] = hashlib.sha256((repository / name).read_bytes()).hexdigest()


def command(argv, cwd, log):
    result = subprocess.run(argv, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=120)
    log.write("$ " + " ".join(map(str, argv)) + "\n" + result.stdout)
    if result.returncode:
        raise RuntimeError("Build/patch failure is not accepted as behavioral red: " + result.stdout)


variants = [
    ("corrected", None, "", "observed defaults: dp"),
    ("native-paused-terminal-bug", "baseline-native-terminal.patch", "transition.native_paused_terminal", "native paused terminal restored before display cleanup: 0"),
    ("native-active-terminal-bug", "baseline-native-terminal.patch", "transition.native_active_terminal", "native active terminal process audio retired: 0; restored after joins: 0"),
    ("native-input-only-bug", "baseline-native-input-only.patch", "transition.native_input_only", "native input-only observed audio constructions: 1"),
    ("first-assignment-bug", "baseline-first-assignment.patch", "roles.pre_display_first_sink", "observed defaults: fallback"),
    ("no-early-hold-bug", "baseline-no-early-hold.patch", "roles.pre_display_first_sink", "observed defaults: fallback"),
    ("stale-user-choice-bug", "baseline-stale-user-choice.patch", "roles.newer_choices", "observed communications choice: headset"),
    ("rtsp-early-release-bug", "baseline-release-before-join.patch", "context.rtsp_join", "rtsp observed control destruction at join: 1"),
    ("webrtc-early-release-bug", "baseline-release-before-join.patch", "context.webrtc_join", "webrtc observed control destruction at join: 1"),
    ("video-owner-unwind-bug", "baseline-owner-order.patch", "--owner-unwind-video", "owner unwind observed destruction: 1; hold=released"),
    ("audio-owner-unwind-bug", "baseline-owner-order.patch", "--owner-unwind-audio", "owner unwind observed destruction: 1; hold=released"),
    ("failed-start-hold-bug", "baseline-start-hold-publication.patch", "context.start_hold_failure", "failed audio start after video=yes observed destruction: 0"),
    ("config-before-hold-bug", "baseline-config-prehold.patch", "transition.config_prehold", "config transition observed defaults: fallback"),
    ("stale-effective-config-bug", "baseline-config-install-order.patch", "transition.config_prehold", "config transition observed defaults: fallback"),
    ("disarm-before-hold-bug", "baseline-disarm-prehold.patch", "transition.first_disarm", "first DISARM observed capture count: 0"),
    ("rollback-hold-copy-bug", "baseline-request-hold-copy.patch", "transition.rollback_hold", "rollback observed control destruction: 1"),
    ("capture-veto-bug", "baseline-capture-veto.patch", "transition.capture_failure", "capture exception observed REVERT count: 0"),
    ("first-fallback-veto-bug", "baseline-fallback-capture-veto.patch", "transition.fallback_first_failure", "fallback first failure observed attempts: 1; preparation=0; escaped=1"),
    ("repeated-fallback-veto-bug", "baseline-fallback-capture-veto.patch", "transition.fallback_repeated_failure", "fallback repeated failure observed attempts: 3; preparation=0; escaped=1"),
]
for label, patch, selection, observed in variants:
    checkout = output / label
    for name in files:
        target = checkout / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repository / name, target)
    with (output / (label + ".log")).open("w") as log:
        if patch:
            patch_path = repository / "tests/audio_snapshot" / patch
            manifest["inputs"][str(patch_path.relative_to(repository))] = hashlib.sha256(patch_path.read_bytes()).hexdigest()
            command(["git", "apply", str(patch_path)], checkout, log)
        build = checkout / "build"
        command(["cmake", "-S", "tests/audio_snapshot", "-B", str(build), "-G", "Ninja",
                 "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DCMAKE_CXX_COMPILER=" + args.compiler], checkout, log)
        command(["cmake", "--build", str(build), "-j2"], checkout, log)
        result = subprocess.run([str(build / "audio_snapshot_tests"), selection],
                                text=True, capture_output=True, timeout=30)
        log.write(result.stdout + result.stderr)
        expected = 1 if patch else 0
        if result.returncode != expected or observed not in result.stdout + result.stderr:
            raise RuntimeError(label + " did not produce the expected observable behavior: " + result.stdout + result.stderr)
        if not patch:
            for mode in ("--owner-unwind-video", "--owner-unwind-audio"):
                command([str(build / "audio_snapshot_tests"), mode], checkout, log)
        manifest["runs"].append({"variant": label, "selection": selection, "exit_code": result.returncode,
                                  "observed": observed, "log": str(output / (label + ".log"))})
        print(label + ": " + observed + ", exit=" + str(result.returncode))
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
