#!/usr/bin/env python3
"""Build and run the application's actual process-catalog component.

No production source is copied, extracted, rewritten or truncated. The C++ test
links src/process_catalog.cpp and uses real production contexts/environments and
standard mutexes; only file parsing, OS process teardown and task time are seams.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("repo", type=Path)
parser.add_argument("--compiler", default="g++")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--boost-process-library", default="boost_process", help="Boost.Process library name or full path")
parser.add_argument("--scenario", help="Run one named C++ scenario")
parser.add_argument("--build-dir", type=Path, help="Retain the linked objects and executable for inspection")
args = parser.parse_args()


def run(directory):
    directory.mkdir(parents=True, exist_ok=True)
    production = args.repo.resolve() / "src/process_catalog.cpp"
    test = Path(__file__).resolve().with_name("test_process_catalog.cpp")
    common = [args.compiler, "-std=c++23", "-pthread", "-g", "-O0", "-I", str(args.repo.resolve())]
    if args.sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    objects = []
    for source in (production, test):
        obj = directory / (source.stem + ".o")
        print(f"Compiling production unit: {source}" if source == production else f"Compiling behavioral tests: {source}", flush=True)
        subprocess.run([*common, "-c", str(source), "-o", str(obj)], check=True, timeout=90)
        objects.append(str(obj))
    executable = directory / "test_process_catalog"
    library = args.boost_process_library if Path(args.boost_process_library).is_absolute() else "-l" + args.boost_process_library
    subprocess.run([*common, *objects, library, "-o", str(executable)], check=True, timeout=30)
    scenarios = [args.scenario] if args.scenario else subprocess.check_output([str(executable), "--list"], text=True).splitlines()
    for scenario in scenarios:
        try:
            subprocess.run([str(executable), scenario], check=True, timeout=5)
        except subprocess.TimeoutExpired:
            raise SystemExit(f"{scenario}: FAIL: production component deadlocked (5 second process timeout)")
    print(f"{len(scenarios)} direct production scenarios passed", flush=True)


if args.build_dir:
    run(args.build_dir.resolve())
else:
    with tempfile.TemporaryDirectory(prefix="process-catalog-") as directory:
        run(Path(directory))
