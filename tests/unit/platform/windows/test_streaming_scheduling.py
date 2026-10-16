#!/usr/bin/env python3
"""Exercise unmodified Windows streaming callbacks against fake OS boundaries.

No Windows APIs or scheduling settings are used on the test host. Each scenario
gets a fresh process, including production globals and once_flags. --source-file
can replay a historical misc.cpp as a negative control with the same fixture.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


SCENARIOS = (
    "native", "multi-owner", "winmm-multi-owner", "threaded-owners", "query-fallback", "set-fallback",
    "no-timer", "native-release-retry", "winmm-release-retry",
    "native-release-new-cycle", "winmm-release-new-cycle", "priority-query-fails",
    "priority-set-fails", "priority-restore-retry", "priority-restore-new-cycle",
    "priority-cleanup-query-fails", "external-priority", "external-priority-after-failure",
    "unmatched-stop", "foreign-native", "foreign-winmm",
    "balanced-cycles", "mixed-backends", "mouse-keys", "paused-watchdog",
    "priority-normal", "priority-above-normal", "priority-below-normal",
    "priority-idle", "priority-high", "priority-realtime",
)


def section(source, start, end):
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def scheduling_helpers(source):
    # Select the scheduling declarations/functions rather than the entire
    # anonymous namespace, which also owns Apollo's independent screen saver.
    names = ("used_nt_set_timer_resolution", "streaming_scheduling_mutex",
             "streaming_scheduling_users", "owned_nt_timer_resolution",
             "owned_time_period", "previous_priority_class")
    declaration_type = r"(?:std::(?:atomic|optional)<[^>]+>|std::mutex|std::size_t|bool)"
    declarations = [line for line in source.splitlines()
                    if any(re.fullmatch(r"  " + declaration_type + " " + name + r"(?: = [^;]+)?;", line)
                           for name in names)]
    result = "namespace {\nusing namespace std::literals;\n" + "\n".join(declarations) + "\n"
    for prefix in ("bool nt_set_timer_resolution_max", "bool nt_set_timer_resolution_min",
                   "void acquire_streaming_scheduling", "void release_streaming_scheduling"):
        marker = "  " + prefix + "("
        if marker in source:
            begin = source.index(marker)
            end = source.index("\n  }\n", begin) + len("\n  }\n")
            result += source[begin:end]
    return result + "}\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repository", type=Path)
    parser.add_argument("compiler", nargs="?", default="c++")
    parser.add_argument("--source-file", type=Path)
    parser.add_argument("--generate-only", type=Path,
                        help="Write the extracted code without compiling or running it")
    args = parser.parse_args()
    path = args.source_file or args.repository / "src/platform/windows/misc.cpp"
    source = path.read_text()
    # Keep complete production function bodies, state, and typed WLAN pointers.
    # Only external Windows/helper/profile boundaries are supplied by the fixture.
    production = scheduling_helpers(source)
    production += "\nnamespace platf {\nvoid enable_mouse_keys();\n"
    last_global = "  decltype(WlanSetInterface) *fn_WlanSetInterface = nullptr;"
    production += section(source, "  std::mutex mouse_keys_mutex;",
                          last_global) + last_global + "\n"
    production += section(source, "  void streaming_will_start() {",
                          "  void restart_on_exit() {")
    production += "\n}\n"
    if args.generate_only:
        args.generate_only.write_text(production)
        return
    fixture = Path(__file__).with_name("streaming_scheduling_fixture.cpp")
    with tempfile.TemporaryDirectory(prefix="streaming-scheduling-") as temporary:
        directory = Path(temporary)
        (directory / "production.inc").write_text(production)
        binary = directory / "streaming-scheduling"
        subprocess.run([args.compiler, "-std=c++23", "-pthread", "-Wall", "-Wextra",
                        "-Werror", "-I", str(directory), str(fixture), "-o", str(binary)],
                       check=True, timeout=30)
        failures = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(binary), scenario], timeout=5, check=False)
            if result.returncode:
                failures.append(scenario)
        print(f"Production streaming scheduling: {len(SCENARIOS) - len(failures)}/"
              f"{len(SCENARIOS)} scenarios passed", flush=True)
        if failures:
            raise SystemExit("Failed scenarios: " + ", ".join(failures))


if __name__ == "__main__":
    main()
