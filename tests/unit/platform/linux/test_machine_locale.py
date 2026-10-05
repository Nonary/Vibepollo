#!/usr/bin/env python3
"""Exercise locale-sensitive metadata guards and dropped-UID helper cwd.

Only fixture files are touched. stat preserves real types/modes/link counts,
but supplies root ownership and translated type names without needing root or
installed language packs. System services and package managers are never run.
"""

import os
import pathlib
import pwd
import re
import shlex
import shutil
import subprocess
import sys
import tempfile


root = pathlib.Path(sys.argv[1]).resolve()
linux = root / "packaging/linux"
brand = "vibepollo"
bash = shutil.which("bash")


def function(source, name):
    match = re.search(r"^" + re.escape(name) + r"\(\) ([{(])", source, re.M)
    if not match:
        raise AssertionError(f"missing function: {name}")
    closing = "}" if match[1] == "{" else ")"
    if source[match.end()] != "\n":
        return source[match.start():source.index("\n", match.end())]
    end = source.index("\n" + closing + "\n", match.end()) + 2
    return source[match.start():end]


def prologue(source):
    """Execute the actual entrypoint header, stopping before any host actions."""
    lines = []
    for line in source.splitlines():
        if (not line.strip() or line.lstrip().startswith("#")
                or line.startswith(("set ", "export ", "umask "))):
            lines.append(line)
        else:
            break
    return "\n".join(lines) + "\n"


with tempfile.TemporaryDirectory(prefix=f"{brand}-locale-test-") as directory:
    fixture = pathlib.Path(directory)
    stat_helper = fixture / "stat"
    stat_helper.write_text(f"#!{sys.executable}\n" + r'''
import os, pathlib, stat, sys
fmt = sys.argv[sys.argv.index('-c') + 1]
metadata = pathlib.Path(sys.argv[-1]).lstat()
kind = ('regular file' if stat.S_ISREG(metadata.st_mode) else
        'directory' if stat.S_ISDIR(metadata.st_mode) else 'symbolic link')
if os.environ.get('LC_ALL') != 'C':
    kind = {'regular file': 'fichero regular', 'directory': 'directorio',
            'symbolic link': 'enlace simbólico'}[kind]
values = {'U': 'root', 'G': 'root', 'u': os.environ.get('FIXTURE_UID', '0'),
          'g': '0', 'a': format(stat.S_IMODE(metadata.st_mode), 'o'),
          'h': str(metadata.st_nlink), 'F': kind}
for key, value in values.items():
    fmt = fmt.replace('%' + key, value)
print(fmt)
''')
    stat_helper.chmod(0o755)
    env = dict(os.environ, LC_ALL="C.UTF-8", LANG="es_ES.UTF-8", LANGUAGE="es",
               STAT_HELPER=str(stat_helper))
    # Confirm the fixture really reproduces the translated output that broke
    # production, rather than silently supplying English on every invocation.
    probe = subprocess.check_output([str(stat_helper), "-c", "%F", str(stat_helper)], env=env, text=True)
    assert probe.strip() == "fichero regular"

    def run(script, cwd=fixture):
        subprocess.run([bash, "-eu", "-c", script], cwd=cwd, env=env, check=True)

    fixture_stat = 'fixture_stat() { "$STAT_HELPER" "$@"; }\n'
    controller = (linux / f"{brand}-session-controller").read_text()
    policy = fixture / "machine.conf"
    policy.write_text("desktop_user=alice\n")
    policy.chmod(0o600)
    script = prologue(controller) + fixture_stat + "\n".join(
        function(controller, name).replace("/usr/bin/stat", "fixture_stat")
        .replace("/usr/bin/getent", "fixture_getent")
        for name in ("valid_user", "valid_uid", "read_settings"))
    script += f"\nsettings_file={shlex.quote(str(policy))}\n"
    script += r'''
fixture_getent() { printf 'alice:x:1000:1000:Alice:/home/alice:/bin/bash\n'; }
read_settings
[[ "$desktop_user:$desktop_uid" == alice:1000 ]]
# Translation must never bypass actual permission or policy failures.
chmod 644 "$settings_file"
if read_settings; then exit 81; fi
chmod 600 "$settings_file"
printf 'desktop_user=alice\ndesktop_user=bob\n' >"$settings_file"
if read_settings; then exit 82; fi
printf 'desktop_user=alice\n' >"$settings_file"
mv "$settings_file" "$settings_file.real"
ln -s "$settings_file.real" "$settings_file"
if read_settings; then exit 83; fi
'''
    run(script)
    print("PASS: controller accepts translated-locale policy and rejects unsafe policy")

    rpm = (linux / "copr/Sunshine.spec").read_text()
    lifecycle_sources = [("Arch install/upgrade/remove", (linux / f"Arch/{brand}.install").read_text())]
    lifecycle_sources += [(name, (linux / f"vibeshine-{name}.in").read_text())
                          for name in ("preinst", "postinst", "prerm")]
    for section, following in (("pre", "post"), ("post", "preun"), ("preun", "files")):
        source = rpm.split(f"\n%{section}\n", 1)[1].split(f"\n%{following}\n", 1)[0]
        lifecycle_sources.append((f"RPM {section}", source.replace("%%", "%")))
    helper = fixture / "helper"
    for label, source in lifecycle_sources:
        helper.write_text("#!/bin/sh\nexit 0\n")
        helper.chmod(0o755)
        name = f"{brand}_privileged_helper_is_safe"
        if label == "RPM preun":
            name = f"{brand}_preun_privileged_helper_is_safe"
        script = prologue(source) + fixture_stat
        script += function(source, name).replace("stat -c", "fixture_stat -c")
        script += f"\nhelper={shlex.quote(str(helper))}\n"
        script += f'''
{name} "$helper"
chmod 777 "$helper"
if {name} "$helper"; then exit 84; fi
chmod 755 "$helper"
export FIXTURE_UID=1000
if {name} "$helper"; then exit 85; fi
unset FIXTURE_UID
ln -s "$helper" "$helper.link"
if {name} "$helper.link"; then exit 86; fi
rm "$helper.link"
ln "$helper" "$helper.hardlink"
if {name} "$helper"; then exit 87; fi
rm "$helper.hardlink"
'''
        run(script)
        print(f"PASS: {label} helper guard accepts localized install and rejects unsafe files")

    # Run Arch's real hook orchestration in installation/removal/reinstallation
    # order. Privileged operations are faked; the locale-sensitive helper guard,
    # setup branch and production pre_remove/post_install functions are real.
    arch = (linux / f"Arch/{brand}.install").read_text()
    helpers = fixture / "helpers"
    helpers.mkdir()
    for name in (f"{brand}-machine-host", "vibeshine-drm-install", "vibeshine-ds5-install"):
        path = helpers / name
        path.write_text('#!/bin/sh\nprintf "%s %s\\n" "${0##*/}" "$*" >>"$TRACE"\n')
        path.chmod(0o755)
    arch = arch.replace("/usr/libexec/vibeshine/", str(helpers) + "/")
    arch = arch.replace("stat -c", "fixture_stat -c")
    trace = fixture / "lifecycle-trace"
    env["TRACE"] = str(trace)
    script = fixture_stat + arch + f'''
systemctl() {{ printf 'systemctl %s\\n' "$*" >>"$TRACE"; }}
{brand}_quiesce_or_abort() {{
  {brand}_privileged_helper_is_safe {shlex.quote(str(helpers / (brand + '-machine-host')))} || return 1
  printf 'quiesce\\n' >>"$TRACE"
}}
{brand}_unmask_host_for_controller() {{ printf 'unmask\\n' >>"$TRACE"; }}
do_secure_executables() {{ printf 'secure\\n' >>"$TRACE"; }}
do_udev_reload() {{ :; }}
do_restore_kwin_capability() {{ :; }}
do_install_vibeshine_drm() {{ :; }}
do_install_vibeshine_ds5() {{ :; }}
do_print_next_steps() {{ :; }}
printf 'existing-pairings\\n' >pairings
pre_install
post_install
pre_upgrade
post_upgrade
pre_remove
[[ $(cat pairings) == existing-pairings ]]
pre_install
post_install
[[ $(cat pairings) == existing-pairings ]]
'''
    run(script)
    calls = trace.read_text().splitlines()
    assert calls.count("quiesce") == 7, calls
    assert calls.count("secure") == 3, calls
    assert calls.count(f"{brand}-machine-host configure-auto") == 3, calls
    assert calls.count("vibeshine-drm-install remove") == 1, calls
    assert calls.count("vibeshine-ds5-install remove") == 1, calls
    assert calls.count(f"systemctl start {brand}-session-controller.service") == 3, calls
    print("PASS: localized Arch install/upgrade/remove/reinstall orchestration preserves fixture pairings")

    # Execute the actual purge path against fixture-only state, including its
    # translated directory ancestry check. No real /etc or /var paths survive.
    state = fixture / "var/lib" / brand
    state.mkdir(parents=True)
    state.parent.chmod(0o755)
    (state / "pairings").write_text("fixture identity")
    etc = fixture / "etc" / brand
    etc.mkdir(parents=True)
    (etc / "machine.conf").write_text("desktop_user=alice\n")
    purge = (linux / "vibeshine-postrm.in").read_text()
    purge = purge.replace("/var/lib", str(state.parent)).replace("/etc/" + brand, str(etc))
    purge = purge.replace("stat -c", "fixture_stat -c")
    run(fixture_stat + "set -- purge\n" + purge)
    assert not state.exists() and not etc.exists()
    print("PASS: localized purge validates ancestry and removes only fixture state")

    host = (linux / f"{brand}-machine-host").read_text()
    # Simulate an identity drop that refuses find from any inherited cwd other
    # than /. This catches the reported failure without privileged syscalls.
    user_exec = function(host, "user_exec").replace("/usr/bin/id", "fixture_id")
    user_exec = user_exec.replace("/usr/bin/setpriv", "fixture_setpriv")
    run(prologue(host) + user_exec + r'''
fixture_id() { printf '1000\n'; }
fixture_setpriv() {
  [[ "$PWD" == / ]] || { echo 'inaccessible inherited working directory' >&2; return 1; }
  while [[ "$1" != -- ]]; do shift; done
  shift
  "$@"
}
original=$PWD
user_exec alice /usr/bin/find "$original" -maxdepth 0 >/dev/null
[[ "$PWD" == "$original" ]]
# Preserve pipe input, arguments and failure status across the cwd isolation.
[[ $(printf 'input\n' | user_exec alice /usr/bin/cat) == input ]]
if user_exec alice /usr/bin/false; then exit 88; fi
''')
    print("PASS: dropped-UID helper uses / and preserves caller cwd, stdin and failure status")

    # Reproduce GNU find's actual cwd failure without requiring root: remove
    # search permission from a fixture cwd after entering it. Keep real id/find
    # calls; replace only setpriv so an ordinary test user can execute the child.
    if os.geteuid() != 0:
        private = fixture / "private-cwd"
        private.mkdir(mode=0o700)
        real_wrapper = function(host, "user_exec").replace("/usr/bin/setpriv", "fixture_setpriv")
        current_user = pwd.getpwuid(os.getuid()).pw_name
        cwd_script = prologue(host) + real_wrapper + r'''
fixture_setpriv() {
  [[ "$1:$2:$3:$4:$5:$6:$7" == "--reuid:$(id -u):--regid:$(id -g):--init-groups:--no-new-privs:--" ]] || return 90
  shift 7
  "$@"
}
original=$PWD
# Open the error log before making relative pathname lookup inaccessible.
exec 3>find-error
chmod 000 "$original"
trap 'chmod 700 "$original"' EXIT
if /usr/bin/find "$STAT_HELPER" -maxdepth 0 >/dev/null 2>&3; then exit 91; fi
user_exec "$FIXTURE_CURRENT_USER" /usr/bin/find "$STAT_HELPER" -maxdepth 0
[[ "$PWD" == "$original" ]]
'''
        try:
            result = subprocess.run([bash, "-eu", "-c", cwd_script], cwd=private,
                                    env=dict(env, FIXTURE_CURRENT_USER=current_user),
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            assert result.stdout == str(stat_helper) + "\n", result.stdout
            assert result.stderr == "", result.stderr
        finally:
            private.chmod(0o700)
        assert "Failed to restore initial working directory" in (private / "find-error").read_text()
        print("PASS: real find reproduces inaccessible cwd and succeeds through user_exec")

    installer = (root / "scripts/linux_install.sh").read_text()
    run(prologue(installer) + '[[ "$LC_ALL" == C ]]\n')
    print("PASS: top-level installer normalizes parsed command locale")
