#!/usr/bin/env python3
"""An incoming Arch package guard blocks replacement on an unsafe fence path.

The candidate archive, incoming compatibility hook, runtime paths, systemd
command, and existing fence symlink are private fixtures. No native ALPM or
systemd operation is performed.
"""
import os
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else "vibepollo"
assert brand == "vibepollo"

with tempfile.TemporaryDirectory(prefix="vibepollo-installer-fence-failure-") as temporary:
    fixture = pathlib.Path(temporary)
    archive = fixture / "archive"
    archive.mkdir()

    # Keep the production incoming scriptlet intact apart from private path
    # redirection and the test user's ownership mapping.
    guard = (root / f"packaging/linux/Arch/{brand}.install").read_text()
    guard = guard.replace("0:0:", f"{os.getuid()}:{os.getgid()}:")
    guard = guard.replace("/run/", f"{fixture}/run/")
    guard = guard.replace("/etc/systemd/system", f"{fixture}/etc/systemd/system")
    guard = guard.replace("/sys/fs/cgroup", f"{fixture}/sys/fs/cgroup")
    guard = guard.replace("/proc/", f"{fixture}/proc/")
    guard = guard.replace('pathlib.Path("/proc")', f"pathlib.Path({str(fixture / 'proc')!r})")
    guard = guard.replace("/usr/libexec/vibeshine/", f"{fixture}/usr/libexec/vibeshine/")
    guard = guard.replace("/usr/bin/vibepollo", f"{fixture}/usr/bin/vibepollo")
    guard = guard.replace("/usr/bin/python3", shlex.quote(sys.executable))

    # Install's root ownership flags are mapped to the temporary fixture owner;
    # the production metadata checks still run against the resulting files.
    guard += r'''
install() {
  local -a arguments=()
  while (($#)); do
    case "$1" in -o|-g) shift 2 ;; *) arguments+=("$1"); shift ;; esac
  done
  command install "${arguments[@]}"
}
'''

    package_systemd = fixture / "run/systemd/system"
    package_systemd.mkdir(parents=True)
    random_uuid = fixture / "proc/sys/kernel/random/uuid"
    random_uuid.parent.mkdir(parents=True)
    random_uuid.write_text("11111111-2222-3333-4444-555555555555\n")
    fake_bin = fixture / "bin"
    fake_bin.mkdir()
    trace = fixture / "systemctl.trace"
    systemctl = fake_bin / "systemctl"
    systemctl.write_text(f'''#!/usr/bin/env python3
import pathlib
import sys

args = sys.argv[1:]
trace = pathlib.Path({str(trace)!r})
with trace.open("a", encoding="utf-8") as stream:
    stream.write(" ".join(args) + "\\n")

if args and args[0] == "is-enabled":
    print("disabled")
    raise SystemExit(0)
if args and args[0] == "show" and len(args) >= 2:
    unit = args[1]
    values = {{
        "LoadState": "loaded",
        "ActiveState": "inactive",
        "SubState": "dead",
        "MainPID": "0",
        "ControlGroup": "",
        "Job": "",
        "FreezerState": "running",
        "InvocationID": "",
    }}
    properties = [item.partition("=")[2] for item in args[2:] if item.startswith("--property=")]
    for name in properties:
        if name not in values:
            raise SystemExit(91)
        print(f"{{name}}={{values[name]}}")
    raise SystemExit(0)
if args == ["daemon-reload"]:
    raise SystemExit(0)
raise SystemExit(92)
''')
    systemctl.chmod(0o755)

    # A test-user-owned directory symlink occupies the private runtime
    # host-fence drop-in path. Its target must survive rejected preflight.
    target = fixture / "preserved-private-runtime-directory"
    target.mkdir()
    sentinel = target / "sentinel"
    sentinel.write_text("keep\n")
    dropin_directory = package_systemd / f"{brand}.service.d"
    dropin_directory.symlink_to(target, target_is_directory=True)

    migration_dir = archive / f"usr/share/{brand}/installer-migrations"
    migration_dir.mkdir(parents=True)
    (migration_dir / "upgrade-2.0-lifecycle.sh").write_text(
        (root / "scripts/linux/migrations/upgrade-2.0-lifecycle.sh").read_text()
    )
    (archive / ".INSTALL").write_text(guard)
    candidate = fixture / "candidate.pkg.tar"
    subprocess.run(
        ["bsdtar", "-cf", str(candidate), "-C", str(archive), ".INSTALL", "usr"],
        check=True,
    )

    # The exact published 2.0.0 selector fixture supplies the legacy signature
    # that makes stage_upgrade_guard choose the incoming same-name override.
    old_selector = (root / "tests/unit/platform/linux/fixtures/package-2.0.0/arch-selector.sh").read_text()
    outgoing = fixture / "outgoing-2.0.0"
    outgoing.write_text(
        old_selector
        + "\n# historical locale-sensitive root metadata comparison: %F regular file\n"
    )

    replacement = fixture / "package-replaced"
    body = f'''
source {shlex.quote(str(root / "scripts/linux_install.sh"))}
workdir={shlex.quote(str(fixture / "installer-stage"))}
pacman() {{ [ "$*" = '-Q {brand}' ] && printf '%s\\n' '{brand} 2.0.0-1'; }}
stage_upgrade_guard {shlex.quote(str(candidate))} {shlex.quote(str(outgoing))}
[[ -f "$workdir/hooks/00-{brand}-quiesce.hook" ]]
[[ ! -e "$workdir/hooks/00-{brand}-incoming-quiesce.hook" ]]
grep -Fxq AbortOnFail "$workdir/hooks/00-{brand}-quiesce.hook"
preflight=$(sed -n 's/^Exec = //p' "$workdir/hooks/00-{brand}-quiesce.hook")
if "$preflight"; then touch {shlex.quote(str(replacement))}; fi
'''
    environment = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}")
    completed = subprocess.run(["bash", "-c", body], env=environment, capture_output=True, text=True)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert not replacement.exists(), "AbortOnFail fixture must block package replacement"
    assert dropin_directory.is_symlink() and dropin_directory.resolve() == target
    assert sentinel.read_text() == "keep\n", "private runtime fence target was modified"
    output = completed.stdout + completed.stderr
    assert f"failed step: {brand}_prepare_host_upgrade_fence" in output, output
    assert f"error: {brand} host-upgrade fence preparation failed at phase: reject-dropin-directory-symlink" in output, output
    calls = trace.read_text().splitlines()
    assert calls, "the private systemd shim should observe the inactive host/controller proof"
    assert not any(call.split()[0] in {"freeze", "stop", "start", "kill"} for call in calls), calls
    print(
        f"{brand}: migrated 2.0.0 hook selects the incoming AbortOnFail guard; "
        "unsafe private fence symlink blocks replacement before freeze/stop and remains unchanged"
    )
