"""Exercise the production controller and machine-host record parsers as a user."""
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
base = dict(session="c1", generation="1", user="sddm", uid="982", gid="982",
            home="/var/lib/sddm", runtime="/run/user/982", display="wayland-9",
            xdisplay="", xauthority="", groups="982", role="greeter", backend="kwin",
            compositor="sddm-42-123")
cases = [(True, {}), (False, dict(role="desktop")),
         (False, dict(user="plasmalogin")), (False, dict(backend="hyprland-kms"))]
for token in ("sddm-0-123", "sddm-42-0", "sddm-042-123", "sddm-42-0123",
              "sddm--42-123", "sddm-42-123-extra", "sddm-42", "other-42-123"):
    cases.append((False, dict(compositor=token)))
cases.extend([(True, dict(user="plasmalogin", compositor="")),
              (True, dict(role="desktop", user="fixture", compositor="",
                          xdisplay=":0", xauthority="/run/user/982/xauth_test"))])

with tempfile.TemporaryDirectory(prefix="vp-sddm-records-") as directory:
    record = pathlib.Path(directory) / "session.env"
    for script_name, function_name in (("vibepollo-session-controller", "read_session_record"),
                                       ("vibepollo-machine-host", "load_session")):
        source = root / "packaging/linux" / script_name
        text = source.read_text()
        start = text.index(function_name + "() {\n")
        end = text.index("\n}\n", start) + 3
        # Redirect the one root record path; keep the full production parser.
        function = text[start:end].replace("$session_file", "$fixture_file")
        for accepted, changes in cases:
            fields = dict(base, **changes)
            record.write_text("".join(f"{key}={value}\n" for key, value in fields.items()))
            harness = f"""
source {shlex.quote(str(source))}
fixture_file={shlex.quote(str(record))}
/usr/bin/stat() {{ printf 'root:vibepollo:640\\n'; }}
/usr/bin/getent() {{ printf '%s:x:982:982::/var/lib/sddm:/usr/bin/nologin\\n' {shlex.quote(fields['user'])}; }}
{function}
declare -A parsed
{function_name} {'parsed' if function_name == 'read_session_record' else ''}
"""
            result = subprocess.run(["bash", "-c", harness], text=True, capture_output=True)
            assert (result.returncode == 0) == accepted, (script_name, changes, result.returncode, result.stderr)
print("PASS: controller and machine-host SDDM record admission, role isolation and legacy KWin compatibility")
