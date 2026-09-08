#!/usr/bin/env python3
"""Exercise production upgrade control flow with isolated system-operation fakes.

No root access or Linux host mutation is needed. The confined importer and
ownership syscalls still require separate integration testing on Linux.
"""
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
source = (root / "packaging/linux/vibepollo-machine-host").read_text()


def function(name):
    start = source.index(name + "() {\n")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


with tempfile.TemporaryDirectory() as directory:
    fixture = pathlib.Path(directory)
    # Source only the relevant production functions, allowing these control-flow
    # tests to run on macOS's Bash as well as Linux. System calls are faked;
    # profile files, markers, and symlinks below are real temporary files.
    script = "\n".join(function(name) for name in (
        "prepare_upgrade_profile", "migrate_profile", "configure_automatically",
        "read_desktop_user",
    ))
    script = script.replace("/usr/bin/stat", "fixture_stat").replace("/usr/bin/getent", "fixture_getent")
    script += r'''
fail() { printf '%s\n' "$*" >&2; return 1; }
require_root() { return 0; }
load_settings() { return 0; }
fixture_getent() { return 0; }
fixture_stat() {
  if [[ "${@: -1}" == /var/lib ]]; then printf 'root:root:755:directory\n'
  elif [[ "$fail_at" == policy ]]; then printf 'alice:alice:644:regular file\n'
  else printf 'root:root:600:regular file\n'; fi
}
service_user=vibepollo
import_source=auto
machine_host=/usr/libexec/vibeshine/vibepollo-machine-host
machine_profile=$PWD/machine
profile_marker=$machine_profile/.machine-profile
settings_file=$PWD/machine.conf
legacy_settings_file=$PWD/prelogin.conf
printf 'desktop_user=alice\n' >"$settings_file"
write_settings() { [[ "$1" == alice && "$fail_at" != settings ]]; }
profile_tree_is_sanitized() { [[ "$fail_at" != validate ]]; }
repair_profile_tree() { printf 'repair\n' >>trace; [[ "$fail_at" != repair ]]; }
user_exec() {
  [[ "$1" == vibepollo && "$2" == "$machine_host" && "$3" == prepare-profile ]] || exit 80
  printf 'prepare\n' >>trace
  [[ "$fail_at" != prepare ]]
}
ensure_command_manifest() { printf 'manifest\n' >>trace; [[ "$fail_at" != manifest ]]; }
publish_migrated_profile() {
  printf 'import\n' >>trace
  [[ "$fail_at" != import ]] || return 1
  [[ ! -e "$machine_profile" ]] || return 1
  cp -R legacy "$machine_profile" || return 1
  printf 'source_user=alice\n' >"$profile_marker"
}
assert_trace() { [[ "$(cat trace)" == "$1" ]] || { cat trace; exit 81; }; }
mkdir legacy
printf 'paired-identity-and-credentials\n' >legacy/sunshine_state.json
printf 'user-settings\n' >legacy/vibepollo.conf
printf 'applications\n' >legacy/apps.json
fail_at=''
: >trace
configure_automatically || exit 82
assert_trace $'repair\nimport\nprepare\nmanifest'
diff legacy/sunshine_state.json "$machine_profile/sunshine_state.json" || exit 83
diff legacy/vibepollo.conf "$machine_profile/vibepollo.conf" || exit 84
diff legacy/apps.json "$machine_profile/apps.json" || exit 85
# The old source remains a backup. Newer machine data must win on every rerun.
printf 'new-pairing\n' >"$machine_profile/sunshine_state.json"
for pass in 1 2; do
  : >trace
  if configure_automatically; then :; else exit 86; fi
  assert_trace $'repair\nprepare\nmanifest'
  [[ "$(cat "$machine_profile/sunshine_state.json")" == new-pairing ]] || exit 87
done
# Failure checks deliberately call the function in an if, disabling Bash's
# implicit errexit. Every failed stage must still stop all subsequent stages.
for fail_at in policy settings repair validate prepare manifest; do
  : >trace
  if configure_automatically; then echo "unexpected success: $fail_at"; exit 88; fi
  case "$fail_at" in
    policy|settings) assert_trace '' ;;
    repair|validate) assert_trace repair ;;
    prepare) assert_trace $'repair\nprepare' ;;
    manifest) assert_trace $'repair\nprepare\nmanifest' ;;
  esac
done
# Malformed administrator policy must not continue to a profile import/repair.
fail_at=''
for policy in 'unknown=value' $'desktop_user=alice\ndesktop_user=bob' '# no selected user'; do
  printf '%s\n' "$policy" >"$settings_file"
  : >trace
  if configure_automatically; then exit 92; fi
  assert_trace ''
done
printf 'desktop_user=alice\n' >"$settings_file"
rm -rf "$machine_profile"
fail_at=import
: >trace
if configure_automatically; then exit 89; fi
assert_trace $'repair\nimport'
[[ ! -e "$profile_marker" && -f legacy/sunshine_state.json ]] || exit 90
# A symlink marker is never treated as a completed migration or replaced.
fail_at=''
mkdir "$machine_profile"
ln -s "$PWD/legacy/sunshine_state.json" "$profile_marker"
: >trace
if migrate_profile; then exit 91; fi
assert_trace ''
printf 'PASS: profile upgrade preservation, repeatability, and failure handling\n'
'''
    subprocess.run([shutil.which("bash"), "-eu", "-c", script], cwd=fixture, check=True)
