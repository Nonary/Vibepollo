#!/usr/bin/env python3
"""Compile controlled CPU witnesses against exact production protocol bodies.

No Windows aliases, native process, device, display or helper execution is used.
The SDK-dependent branches are excluded; these are protocol/lifetime witnesses.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile


def definition(text, marker):
    if text.count(marker) != 1:
        raise RuntimeError(f"expected one actual definition: {marker}")
    start = text.index(marker)
    opening = text.index(") {", start) + 2
    depth = 0
    for end in range(opening, len(text)):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                return text[start:end + 1]
    raise RuntimeError(f"unterminated actual definition: {marker}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--core-root", type=Path, help="exact core-header root for a separately composed Windows leaf")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--prepare-only", action="store_true")
    args = parser.parse_args()
    repo = args.repo.resolve()
    core = (args.core_root or repo).resolve()
    if not (core / "src/display_target_lease.h").is_file():
        parser.error("compose the real display_target_lease.h or provide its exact --core-root")
    output = args.build_dir or Path(tempfile.mkdtemp(prefix="display-helper-borrow-witness-"))
    output.mkdir(parents=True, exist_ok=True)
    sources = {
        "client": repo / "src/platform/windows/ipc/display_settings_client.cpp",
        "worker": repo / "src/platform/windows/display_helper_v2/async_dispatcher.cpp",
    }
    client = sources["client"].read_text()
    worker = sources["worker"].read_text()
    (output / "actual-client.inc").write_text("\n".join(definition(client, marker) for marker in [
        "    std::optional<display_helper::borrow::result_t> exchange_borrow_control(",
        "  bool release_borrow_quiescence(",
        "  std::optional<display_helper::borrow::result_t> acquire_borrow_quiescence(",
    ]) + "\n")
    (output / "actual-worker.inc").write_text("\n".join(definition(worker, marker) for marker in [
        "  void AsyncDispatcher::enqueue_task(", "  void AsyncDispatcher::worker_loop(",
    ]) + "\n")
    inputs = list(sources.values()) + [core / "src/display_target_lease.h",
        repo / "tests/unit/test_display_helper_borrow_quiescence.cpp"]
    inputs += list((repo / "src/platform/windows").glob("display_helper_borrow_*.h"))
    inputs += [output / "actual-client.inc", output / "actual-worker.inc"]
    records = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    (output / "identities.json").write_text(json.dumps(records, indent=2) + "\n")
    if args.prepare_only:
        print(output)
        return
    command = [args.cxx, "-std=c++20", "-O0", "-g0", "-pthread", f"-I{repo}", f"-I{core}", f"-I{output}",
        str(repo / "tests/unit/test_display_helper_borrow_quiescence.cpp"), "-o", str(output / "borrow-witness")]
    print(json.dumps({"compile": command, "scope": "controlled CPU leaves only; no whole Windows TU/native runtime claim"}))
    subprocess.run(command, check=True)
    subprocess.run([str(output / "borrow-witness")], check=True, timeout=20)


if __name__ == "__main__":
    main()
