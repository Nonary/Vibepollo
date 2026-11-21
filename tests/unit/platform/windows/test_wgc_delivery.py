#!/usr/bin/env python3
"""Drive the real helper delivery loop through scripted keyed-mutex ownership.

The replay uses the production scratch handoff, direct path, and publication
code. D3D textures hold a distinct marker value and a fake IPC consumer
observes the shared texture and its record at each frame-ready signal.
--revision selects both the helper and policy from an older local Git revision.
"""

import argparse
from pathlib import Path
import subprocess
import tempfile

from test_wgc_source_admission import block


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--revision")
    args = parser.parse_args()
    args.repository = args.repository.resolve()

    def read_source(path):
        if args.revision:
            return subprocess.check_output(
                ["git", "-C", str(args.repository), "show", f"{args.revision}:{path}"],
                text=True,
            )
        return (args.repository / path).read_text()

    source = read_source("tools/sunshine_wgc_capture.cpp")
    policy = read_source("src/platform/windows/wgc_capture_policy.h")
    markers = [
        "  enum class scratch_state_e",
        "  struct scratch_texture_t",
        "  struct delivery_frame_t",
        "  std::optional<size_t> reserve_scratch_texture(",
        "  bool enqueue_scratch_texture(",
        "  bool try_publish_directly(",
        "  void delivery_thread_proc(",
        ("  void copy_pending_frame_to_shared_texture("
         if "  void copy_pending_frame_to_shared_texture(" in source
         else "  void copy_frame_to_shared_texture("),
    ]
    extracted = "\n".join(
        block(source, marker) + (";" if marker.startswith(("  struct", "  enum")) else "")
        for marker in markers
    )
    fixture = Path(__file__).with_name("fixtures") / "wgc_delivery_harness.cpp.in"
    code = fixture.read_text().replace("// PRODUCTION_METHODS", extracted)

    with tempfile.TemporaryDirectory(prefix="wgc-delivery-") as directory:
        root = Path(directory)
        (root / "wgc_capture_policy.h").write_text(policy)
        cpp = root / "replay.cpp"
        cpp.write_text(code)
        executable = root / "replay"
        subprocess.run(
            [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread", str(cpp), "-o", str(executable)],
            check=True,
        )
        return subprocess.run([str(executable)], timeout=5).returncode


if __name__ == "__main__":
    raise SystemExit(main())
