#!/usr/bin/env python3
"""Installed ALPM guard contract and private complete-library execution.

CMake wiring is inspected, not compiled or installed. The replacement boundary
is modeled from AbortOnFail, and all runtime/helper/proc paths are private.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
brand = sys.argv[2] if len(sys.argv) > 2 else "vibeshine"
assert brand in ("vibepollo", "vibeshine")
arch = root / "packaging/linux/Arch"
hook = (arch / f"00-{brand}-quiesce.hook").read_text()
helper = (arch / f"{brand}-package-preflight").read_text()
cmake = (root / "cmake/packaging/linux.cmake").read_text()
for line in ("Operation = Install", "Operation = Upgrade", "Operation = Remove", "Type = Package", f"Target = {brand}", "When = PreTransaction", f"Exec = /usr/libexec/vibeshine/{brand}-package-preflight", "AbortOnFail"):
    assert line in hook.splitlines(), line
assert f'. /usr/share/{brand}/arch-package-hooks || exit 1' in helper
assert helper.rstrip().endswith(f"{brand}_quiesce_or_abort")
blocks = re.findall(r'install\([^)]*\)', cmake, re.S)
assert any("PROGRAMS" in b and f"Arch/{brand}-package-preflight" in b and 'DESTINATION "${VIBESHINE_PRIVILEGED_LIBEXEC_INSTALL_DIR}"' in b for b in blocks)
assert any("FILES" in b and f"Arch/{brand}.install" in b and f'${{CMAKE_INSTALL_DATADIR}}/{brand}' in b and 'RENAME "arch-package-hooks"' in b for b in blocks)
assert any("FILES" in b and f"Arch/00-{brand}-quiesce.hook" in b and '${CMAKE_INSTALL_DATADIR}/libalpm/hooks' in b for b in blocks)
assert f"install={brand}.install" in (arch / "PKGBUILD").read_text()
spec = (root / "packaging/linux/copr/Sunshine.spec").read_text()
rpm_install = spec.split("\n%install\n", 1)[1].split("\n%pre\n", 1)[0]
for path in (
    f"%{{_prefix}}/libexec/vibeshine/{brand}-package-preflight",
    f"%{{_datadir}}/libalpm/hooks/00-{brand}-quiesce.hook",
    f"%{{_datadir}}/{brand}/arch-package-hooks",
):
    assert f"rm -f %{{buildroot}}{path}" in rpm_install.splitlines(), path
assert "rmdir %{buildroot}%{_datadir}/libalpm/hooks %{buildroot}%{_datadir}/libalpm 2>/dev/null || true" in rpm_install.splitlines()
source = (arch / f"{brand}.install").read_text()
count = 0
for operation in ("Install", "Upgrade", "Remove"):
    for scenario in ("idle", "verified-old-owner", "missing-library"):
        with tempfile.TemporaryDirectory(prefix=f"{brand}-installed-guard-") as tmp:
            stage = Path(tmp)
            (stage / "proc").mkdir()
            library = stage / "arch-package-hooks"
            code = source.replace("0:0:", f"{os.getuid()}:{os.getgid()}:")
            code = code.replace(f"/run/{brand}", str(stage / "runtime"))
            code = code.replace("/run/systemd/system", str(stage / "absent-systemd"))
            code = code.replace(f"/usr/libexec/vibeshine/{brand}", str(stage / "absent-helper"))
            code = code.replace(f"/usr/bin/{brand}", str(stage / "absent-public"))
            code = code.replace('pathlib.Path("/proc")', f"pathlib.Path({str(stage / 'proc')!r})")
            code = code.replace('/proc/[0-9]*/exe', str(stage / 'proc') + '/[0-9]*/exe')
            code += '\ncommand() { if [[ "$1" = -v && "$2" = systemctl ]]; then return 1; else builtin command "$@"; fi; }\n'
            if scenario == "verified-old-owner":
                # Positive old-inode evidence is the controlled fault point;
                # the actual wrapper/abort/record pipeline still executes.
                code += f"\n{brand}_package_unmanaged_host_absent() {{ return 1; }}\n"
            if scenario != "missing-library":
                library.write_text(code)
            preflight = stage / "preflight"
            preflight.write_text(helper.replace(f"/usr/share/{brand}/arch-package-hooks", shlex.quote(str(library))))
            preflight.chmod(0o755)
            result = subprocess.run([str(preflight)], capture_output=True, text=True)
            replaced = stage / "replacement"
            # Model only ALPM's already-verified PreTransaction AbortOnFail
            # boundary; this does not claim native package execution.
            if result.returncode == 0:
                replaced.write_text(operation)
            assert replaced.exists() == (scenario == "idle"), (operation, scenario, result.returncode, result.stderr)
            assert (result.returncode == 0) == (scenario == "idle"), result.stderr
            count += 1
print(f"{brand}: installed guard contract and {count} private complete-library boundary cases passed; no native package transaction")
