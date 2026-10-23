#!/usr/bin/env python3
"""Run package kill-mode selection against disposable service/helper fixtures.

Usage: python3 test_package_kill_mode.py [repository] [vibepollo|vibeshine]
Extracts functions only; never sources a package hook or calls live systemctl.
"""
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
BRAND = sys.argv[2] if len(sys.argv) > 2 else "vibepollo"
assert BRAND in ("vibepollo", "vibeshine")


def function(source, suffix):
    match = re.search(rf"^{BRAND}_{suffix}\(\) \{{\n.*?^\}}$", source, re.M | re.S)
    assert match, suffix
    return match.group()


contracts = {
    "mixed": "  trap request_host_shutdown TERM INT HUP\n",
    "control-group": "  trap mark_host_shutdown TERM INT HUP\n",
    "process": "  trap 'forward_host_signal TERM' TERM\n  trap 'forward_host_signal INT' INT\n  trap 'forward_host_signal HUP' HUP\n",
    "unknown": "#!/bin/bash\n# no known shutdown contract\n",
}
# A stopped service must prove an empty cgroup. Disabled alone says nothing
# about whether the service is still active.
cases = [
    ("no-service", "absent", "not-found:inactive:dead", 0, 1, 1, "control-group"),
    ("stopped-unknown", "unknown", "loaded:inactive:dead", 0, 1, 1, "control-group"),
    ("stopped-unsafe", "mixed", "loaded:inactive:dead", 0, 1, 0, "control-group"),
    ("stopped-symlink", "symlink", "loaded:inactive:dead", 0, 1, 1, "control-group"),
    ("failed-empty", "unknown", "loaded:failed:failed", 0, 1, 1, "control-group"),
    ("masked-empty", "unknown", "masked:inactive:dead", 0, 1, 1, "control-group"),
    ("stopped-populated", "unknown", "loaded:inactive:dead", 0, 0, 1, "mixed"),
    ("stale-main-pid", "unknown", "loaded:inactive:dead", 412, 1, 1, "mixed"),
    ("active-mixed", "mixed", "loaded:active:running", 412, 0, 1, "mixed"),
    ("active-control-group", "control-group", "loaded:active:running", 412, 0, 1, "control-group"),
    ("active-process", "process", "loaded:active:running", 412, 0, 1, "process"),
    ("active-unsafe", "mixed", "loaded:active:running", 412, 0, 0, None),
    ("active-symlink", "symlink", "loaded:active:running", 412, 0, 1, None),
    ("active-missing", "absent", "loaded:active:running", 412, 0, 1, None),
    ("active-unknown", "unknown", "loaded:active:running", 412, 0, 1, "mixed"),
]
failures = []
count = 0
with tempfile.TemporaryDirectory(prefix=f"{BRAND}-kill-mode-") as directory:
    fixtures = pathlib.Path(directory)
    for name, body in contracts.items():
        path = fixtures / name
        path.write_text(body)
        path.chmod(0o755)
    (fixtures / "symlink").symlink_to(fixtures / "mixed")
    for relative in [f"packaging/linux/Arch/{BRAND}.install", "packaging/linux/vibeshine-preinst.in", "packaging/linux/copr/Sunshine.spec"]:
        source = (ROOT / relative).read_text()
        definitions = "\n".join(function(source, suffix) for suffix in (
            "unit_is_quiescent", "privileged_helper_is_safe", "select_upgrade_kill_mode"))
        if relative.endswith(".spec"):
            definitions = definitions.replace("%%", "%")
        shells = [["bash"]] if "/Arch/" in relative else [["sh"], ["bash", "--posix"]]
        if "/Arch/" not in relative and shutil.which("dash"):
            shells.append([shutil.which("dash")])
        for shell in shells:
            for name, helper, states, pid, empty, safe, expected in cases:
                load, active, substate = states.split(":")
                property_text = f"LoadState={load}\nActiveState={active}\nSubState={substate}\nMainPID={pid}\nControlGroup=/fixture.slice/host.service"
                prelude = f"""
brand={shlex.quote(BRAND)}
fixture={shlex.quote(str(fixtures / helper))}
fixture_properties={shlex.quote(property_text)}
fixture_cgroup_empty={empty}
fixture_helper_safe={safe}
"""
                mocks = r'''
# Only our fixture service may be queried. No live tools execute.
systemctl() {
  [ "$1" = show ] && [ "$2" = "$brand.service" ] || return 99
  printf '%s\n' "$fixture_properties"
}
timeout() { shift 2; "$@"; }
stat() {
  [ "$fixture_helper_safe" = 1 ] || return 1
  case "$2" in
    '%u:%g:%a:%h') printf '%s\n' '0:0:755:1' ;;
    '%u:%g:%a:%h:%F') printf '%s\n' '0:0:755:1:regular file' ;;
    *) return 99 ;;
  esac
}
'''
                body = f"""
{BRAND}_legacy_host=$fixture
{BRAND}_cgroup_is_quiescent() {{ [ "$1" = /fixture.slice/host.service ] && [ "$fixture_cgroup_empty" = 1 ]; }}
if {BRAND}_select_upgrade_kill_mode; then
  printf '0:%s\\n' "${{{BRAND}_upgrade_kill_mode:-unset}}"
else printf '1:blocked\\n'; fi
"""
                completed = subprocess.run(shell + ["-c", prelude + definitions + "\n" + mocks + body], capture_output=True, text=True)
                wanted = f"0:{expected}" if expected else "1:blocked"
                count += 1
                if completed.returncode or completed.stdout.strip() != wanted:
                    failures.append(f"{relative} {' '.join(shell)} {name}: expected {wanted}, got {completed.stdout.strip()} {completed.stderr.strip()}")
for failure in failures:
    print(failure)
print(f"{BRAND}: {count} selector scenarios, {len(failures)} failures")
sys.exit(bool(failures))
