#!/usr/bin/env python3
"""Exercise confined profile copying and machine migration with private fixtures.

The C fixture uses the production openat2/copy/configuration helpers without
root access. Account switching and system installation remain integration work.
"""

import json
import os
import pathlib
import pwd
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


host_path = pathlib.Path(sys.argv[1]).resolve()
importer_path = host_path.with_name("vibepollo-profile-import.c")
host_source = host_path.read_text()


def host_function(name):
    start = host_source.index(name + "() {\n")
    end = host_source.index("\n}\n", start) + 3
    return host_source[start:end]


class ProfileImport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="vibepollo-profile-test-")
        directory = pathlib.Path(cls.build.name)
        harness = directory / "import.c"
        harness.write_text("#define main production_import_main\n#include " +
                           json.dumps(str(importer_path)) + r'''
#undef main
int main(int argc, char **argv) {
  if (argc != 3) return 2;
  umask(0077);
  const int home = open(argv[1], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  const int destination = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (home < 0 || destination < 0) return 3;
  const int source = open_desktop_profile(home);
  if (source < 0) return errno == ENOENT ? 0 : 4;
  struct stat attributes;
  uint64_t maximum_bytes = 0;
  if (fstat(source, &attributes) || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != getuid() ||
      !destination_copy_budget(destination, &maximum_bytes)) return 5;
  const uint64_t now = monotonic_milliseconds();
  struct import_budget budget = {
    .maximum_bytes = maximum_bytes,
    .deadline_milliseconds = now + (uint64_t) maximum_seconds * UINT64_C(1000),
    .source_device = attributes.st_dev,
    .destination_root = destination,
  };
  const bool success = now && copy_directory(source, destination, 0, &budget) &&
                       remap_legacy_configuration(destination);
  close(source);
  close(destination);
  close(home);
  return success ? 0 : 6;
}
''')
        cls.importer = directory / "import"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-O2", str(harness), "-lcap", "-o", str(cls.importer)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="vibepollo-profile-fixture-")
        self.addCleanup(self.temporary.cleanup)
        self.fixture = pathlib.Path(self.temporary.name)
        self.home = self.fixture / "home"
        self.home.mkdir()
        self.incoming = self.fixture / "incoming"
        self.incoming.mkdir(mode=0o700)

    def profile(self, product):
        directory = self.home / ".config" / product
        directory.mkdir(parents=True, exist_ok=True)
        return directory

    def import_profile(self, succeeds=True):
        result = subprocess.run([str(self.importer), str(self.home), str(self.incoming)])
        if succeeds:
            self.assertEqual(result.returncode, 0)
        else:
            self.assertNotEqual(result.returncode, 0)

    def test_legacy_config_pairings_apps_and_covers_survive(self):
        legacy = self.profile("sunshine")
        contents = {"sunshine.conf": "capture = kms\nfps = 144\n",
                    "sunshine_state.json": '{"uniqueid":"paired-host","cert":"credential"}\n',
                    "apps.json": '{"apps":[{"name":"Game"}]}\n',
                    "covers/game.png": "cover bytes\n"}
        for name, value in contents.items():
            target = legacy / name
            target.parent.mkdir(exist_ok=True)
            target.write_text(value)
        self.import_profile()
        for name, value in contents.items():
            imported_name = "vibepollo.conf" if name == "sunshine.conf" else name
            self.assertEqual((self.incoming / imported_name).read_text(), value)
            self.assertEqual((legacy / name).read_text(), value)
            self.assertEqual((self.incoming / imported_name).stat().st_mode & 0o777, 0o600)
        self.assertFalse((self.incoming / "sunshine.conf").exists())

    def test_canonical_source_wins_without_merging_legacy(self):
        primary = self.profile("vibepollo")
        legacy = self.profile("sunshine")
        (primary / "apps.json").write_text("primary applications")
        (legacy / "apps.json").write_text("legacy applications")
        (legacy / "sunshine.conf").write_text("legacy settings")
        (legacy / "sunshine_state.json").write_text("legacy pairing")
        self.import_profile()
        self.assertEqual([path.name for path in self.incoming.iterdir()], ["apps.json"])
        self.assertEqual((self.incoming / "apps.json").read_text(), "primary applications")

    def test_empty_canonical_source_wins(self):
        self.profile("vibepollo")
        (self.profile("sunshine") / "sunshine.conf").write_text("legacy settings")
        self.import_profile()
        self.assertEqual(list(self.incoming.iterdir()), [])

    def test_canonical_config_is_authoritative_in_either_source(self):
        for product in ("vibepollo", "sunshine"):
            with self.subTest(product=product):
                selected = self.profile(product)
                (selected / "vibepollo.conf").write_text("canonical settings")
                (selected / "sunshine.conf").write_text("legacy settings")
                self.import_profile()
                self.assertEqual((self.incoming / "vibepollo.conf").read_text(), "canonical settings")
                self.assertEqual((self.incoming / "sunshine.conf").read_text(), "legacy settings")
                shutil.rmtree(self.home / ".config")
                for path in self.incoming.iterdir():
                    path.unlink()

    def test_unsafe_canonical_paths_never_fall_back(self):
        legacy = self.profile("sunshine")
        (legacy / "sunshine.conf").write_text("legacy settings")
        primary = self.home / ".config/vibepollo"
        for target in (legacy, self.fixture / "missing"):
            with self.subTest(symlink=target):
                primary.symlink_to(target)
                self.import_profile(succeeds=False)
                self.assertEqual(list(self.incoming.iterdir()), [])
                primary.unlink()
        primary.write_text("not a directory")
        self.import_profile(succeeds=False)
        self.assertEqual(list(self.incoming.iterdir()), [])

    @unittest.skipIf(os.geteuid() == 0, "root bypasses directory read permission")
    def test_inaccessible_canonical_source_never_falls_back(self):
        primary = self.profile("vibepollo")
        (self.profile("sunshine") / "sunshine.conf").write_text("legacy settings")
        primary.chmod(0)
        try:
            self.import_profile(succeeds=False)
            self.assertEqual(list(self.incoming.iterdir()), [])
        finally:
            primary.chmod(0o700)

    def test_legacy_symlink_entries_cannot_escape(self):
        legacy = self.profile("sunshine")
        outside = self.fixture / "outside"
        outside.write_text("unrelated secret")
        (legacy / "sunshine.conf").symlink_to(outside)
        self.import_profile(succeeds=False)
        self.assertFalse((self.incoming / "vibepollo.conf").exists())

    def test_absent_sources_start_fresh(self):
        self.import_profile()
        self.assertEqual(list(self.incoming.iterdir()), [])

    def test_privilege_and_confinement_contract(self):
        source = importer_path.read_text()
        main = source.split("int main(int argc, char **argv) {", 1)[1]
        self.assertLess(main.index("if (!drop_to_user(account))"),
                        main.index("const int source = open_desktop_profile(home)"))
        for invariant in ("RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV",
                          "source_attributes.st_uid != account->pw_uid",
                          "PR_SET_NO_NEW_PRIVS", "maximum_file_bytes", "maximum_total_bytes",
                          "same_snapshot(&before, &after)"):
            self.assertIn(invariant, source)

    def test_legacy_and_canonical_cover_paths_use_safe_suffixes(self):
        canonical = self.home / ".config/vibepollo/covers"
        legacy = self.home / ".config/sunshine/covers"
        paths = [str(canonical / "game.png"), str(legacy / "folder/game.png"),
                 str(legacy) + "/../secret.png", str(legacy) + "//game.png",
                 str(canonical) + "/./game.png", str(legacy) + "/",
                 str(self.home / "other/game.png"), "box.png"]
        apps = self.incoming / "apps.json"
        apps.write_text(json.dumps({"apps": [{"image-path": value} for value in paths]}))
        apps.chmod(0o600)
        marker = self.incoming / ".machine-profile"
        marker.write_text("source_user=alice\n")
        marker.chmod(0o600)
        script = f"""
set -euo pipefail
service_user={shlex.quote(pwd.getpwuid(os.getuid()).pw_name)}
machine_profile={shlex.quote(str(self.incoming))}
profile_marker=$machine_profile/.machine-profile
fail() {{ printf '%s\\n' "$*" >&2; return 1; }}
function /usr/bin/getent() {{ printf '%s\\n' {shlex.quote('alice:x:1000:1000::' + str(self.home) + ':/bin/bash')}; }}
{host_function('capability_free_exec')}
{host_function('rewrite_machine_cover_paths')}
rewrite_machine_cover_paths
"""
        subprocess.run([shutil.which("bash"), "-c", script], check=True)
        rewritten = [app["image-path"] for app in json.loads(apps.read_text())["apps"]]
        self.assertEqual(rewritten, [str(self.incoming / "covers/game.png"),
                                     str(self.incoming / "covers/folder/game.png")] + ["box.png"] * 6)

    def test_discovery_selects_legacy_owner_and_respects_source_priority(self):
        homes = {name: self.fixture / name for name in ("alice", "bob")}
        for home in homes.values():
            home.mkdir()
        legacy = homes["alice"] / ".config/sunshine"
        legacy.mkdir(parents=True)
        (legacy / "sunshine_state.json").write_text("paired identity")
        (legacy / "vibeshine_state.json").write_text("duplicate same owner")
        accounts = "\n".join(f"{name}:x:1000:1000::{home}:/bin/bash" for name, home in homes.items())
        script = f"""
set -euo pipefail
settings_file={shlex.quote(str(self.fixture / 'machine.conf'))}
legacy_settings_file={shlex.quote(str(self.fixture / 'prelogin.conf'))}
fail() {{ printf '%s\\n' "$*" >&2; return 1; }}
require_root() {{ return 0; }}
valid_user() {{ [[ "$1" =~ ^[a-z_][a-z0-9_-]*$ ]]; }}
user_exec() {{ shift; "$@"; }}
function /usr/bin/getent() {{ printf '%s\\n' {shlex.quote(accounts)}; }}
write_settings() {{ printf '%s' "$1"; }}
prepare_upgrade_profile() {{ return 0; }}
{host_function('configure_automatically')}
configure_automatically
"""

        def discover(succeeds, selected=""):
            result = subprocess.run([shutil.which("bash"), "-c", script], capture_output=True, text=True)
            self.assertEqual(result.returncode == 0, succeeds, result.stderr)
            self.assertEqual(result.stdout, selected)

        discover(True, "alice")
        canonical = homes["alice"] / ".config/vibepollo"
        canonical.mkdir()
        discover(False)
        (canonical / "sunshine_state.json").write_text("canonical identity")
        discover(True, "alice")
        other = homes["bob"] / ".config/sunshine"
        other.mkdir(parents=True)
        (other / "sunshine_state.json").write_text("other identity")
        discover(False)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
