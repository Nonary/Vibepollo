#!/usr/bin/env python3
"""Verify private force-stop snapshots with real tar and disposable paths."""

import pathlib
import re
import shlex
import subprocess
import sys
import tarfile
import tempfile

root = pathlib.Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else "vibepollo"
assert brand in ("vibepollo", "vibeshine")
paths = [f"packaging/linux/Arch/{brand}.install", "packaging/linux/vibeshine-preinst.in",
         f"packaging/linux/{brand}-prerm.in", "packaging/linux/copr/Sunshine.spec"]
definitions = []
for relative in paths:
    text = (root / relative).read_text()
    matches = re.findall(rf"^{brand}_package_backup_for_force\(\) \{{\n.*?^\}}\n\n{brand}_package_log_force_outcome\(\) \{{\n.*?^\}}$", text, re.M | re.S)
    assert matches, relative
    for match in matches:
        definitions.append(match.replace("%%", "%") if relative.endswith(".spec") else match)
assert all(text == definitions[0] for text in definitions), "standalone hook backups diverged"

with tempfile.TemporaryDirectory(prefix=f"{brand}-force-backup-") as temporary:
    fixture = pathlib.Path(temporary)
    for directory in ("etc", "var", "var/lib"):
        (fixture / directory).mkdir(exist_ok=True)
        (fixture / directory).chmod(0o755)
    config = fixture / f"etc/{brand}"
    profile = fixture / f"var/lib/{brand}"
    config.mkdir()
    profile.mkdir()
    (profile / "logs").mkdir()
    (config / "machine.conf").write_text("fixture-policy\n")
    (profile / "pairings.json").write_text("fixture-pairings\n")
    (profile / "logs/active.log").write_text("changing stream log\n")
    outside = fixture / "unrelated-secret"
    outside.write_text("must-not-be-read\n")
    (profile / "external-link").symlink_to(outside)
    original = definitions[0]
    # Redirect filesystem boundaries only. tar, modes, symlink behavior,
    # archive contents and diagnostic persistence remain the production code.
    definition = original.replace(f"{brand}_backup_base=/var/lib", f"{brand}_backup_base={shlex.quote(str(fixture / 'var/lib'))}")
    definition = definition.replace(f'"/${brand}_backup_source"', f'"{fixture}/${brand}_backup_source"')
    definition = definition.replace('-C / -cf', f'-C {shlex.quote(str(fixture))} -cf')
    definition = definition.replace(f'/var/lib/{brand}-package-backups/forced-stop.*', f'{fixture}/var/lib/{brand}-package-backups/forced-stop.*')
    mocks = r'''
stat() { /usr/bin/stat "$@" | sed -E 's/^[0-9]+:[0-9]+:/0:0:/' ; }
timeout() { while [ "${1#--}" != "$1" ]; do shift; done; shift; "$@"; }
pacman() { [ "$*" = "-Q $brand" ] || return 99; printf '%s 2.0.0-1\n' "$brand"; }
dpkg-query() { return 1; }
rpm() { return 1; }
'''
    prefix = f"brand={brand}\n{brand}_original_invocation=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n{brand}_original_cgroup=/system.slice/{brand}.service\n"

    def run(extra=""):
        return subprocess.run(["sh", "-c", prefix + definition + "\n" + mocks + extra + f"\n{brand}_package_backup_for_force && {brand}_package_log_force_outcome pending && {brand}_package_log_force_outcome complete"],
                              text=True, capture_output=True)

    completed = run()
    assert completed.returncode == 0, completed.stderr
    backups = fixture / f"var/lib/{brand}-package-backups"
    snapshot = next(backups.glob("forced-stop.*"))
    assert snapshot.stat().st_mode & 0o777 == 0o700
    for path in snapshot.iterdir():
        assert path.stat().st_mode & 0o777 == 0o600
    with tarfile.open(snapshot / "machine-state.tar") as archive:
        assert archive.extractfile(f"etc/{brand}/machine.conf").read() == b"fixture-policy\n"
        assert archive.extractfile(f"var/lib/{brand}/pairings.json").read() == b"fixture-pairings\n"
        assert archive.getmember(f"var/lib/{brand}/external-link").issym()
        assert all("active.log" not in member.name and "unrelated-secret" not in member.name for member in archive.getmembers())
    assert "fixture-pairings" not in completed.stderr and "must-not-be-read" not in completed.stderr
    assert f"{brand} 2.0.0-1" in (snapshot / "recovery.log").read_text()
    assert "outcome=pending\noutcome=complete" in (snapshot / "recovery.log").read_text()
    assert run().returncode == 0  # Retry adds a retained snapshot, never overwrites.
    assert len(list(backups.glob("forced-stop.*"))) == 2
    assert run("\ntar() { return 1; }\n").returncode != 0
    backups.chmod(0o755)
    assert run().returncode != 0
    backups.chmod(0o700)
    (profile / "external-link").unlink()
    profile.rename(fixture / "saved-profile")
    profile.symlink_to(fixture / "saved-profile")
    assert run().returncode != 0
    profile.unlink()
    (fixture / "saved-profile").rename(profile)
    backups.rename(fixture / "saved-backups")
    backups.symlink_to(fixture / "saved-backups")
    assert run().returncode != 0

print(f"PASS: {brand} retained private snapshots, links confined, logs excluded, unsafe/failed backups block force")
