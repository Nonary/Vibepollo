"""Exercise the production migration on real temporary extension directories."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[4]
compiler = shutil.which(os.environ.get("CXX", "g++"))
if not compiler:
    raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix="playnite-duplicate-") as temporary:
    root = Path(temporary)
    cpp = root / "migration.cpp"
    cpp.write_text(r'''
#include "src/platform/windows/playnite_plugin_migration.h"
#include <cassert>
#include <iostream>
using namespace platf::playnite::migration;
void manifest(const fs::path &dir, bool script = false) {
  fs::create_directories(dir);
  std::ofstream(dir / "extension.yaml") << (script ?
    "Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: SunshinePlaynite.psm1\n" :
    "Id: Sunshine.Playnite.Integration\nType: GenericPlugin\nModule: VibepolloPlaynite.dll\n");
  std::ofstream(dir / (script ? "SunshinePlaynite.psm1" : "VibepolloPlaynite.dll")) << "module";
}
int main(int, char **argv) {
  fs::path root = argv[1], active = root / "Roaming/Playnite/Extensions/SunshinePlaynite";
  fs::path old = root / "Local/Playnite/Extensions/SunshinePlaynite";
  manifest(active); manifest(old, true);
  std::ifstream unreadable(root / "missing-file");
  assert(read_manifest(unreadable) == manifest_inspection::error);
  std::string error;
  assert(begin_migration(active, error));
  auto read_failure = +[](const fs::path &) { return manifest_inspection::error; };
  assert(!disable_duplicate(active, old, error, read_failure));
  assert(fs::exists(old / "extension.yaml") && migration_pending(active));
  auto metadata_failure = +[](const fs::path &) { return path_inspection::error; };
  assert(!disable_duplicate(active, old, error, inspect_manifest, metadata_failure));
  assert(fs::exists(old / "extension.yaml") && migration_pending(active));
  assert(disable_duplicate(active, old, error));
  assert(complete_migration(active, error) && !migration_pending(active));
  std::error_code marker_ec;
  fs::create_directory_symlink(old, pending_marker(active), marker_ec);
  if (!marker_ec) {
    assert(!begin_migration(active, error));
    assert(fs::exists(old / "extension.yaml.vibepollo-disabled"));
    fs::remove(pending_marker(active));
  }
  manifest(old, true);
  assert(alternate_root(active, active, old) == old);
  assert(alternate_root(old, active, old) == active);
  assert(unresolved_installed_target(active, active, false));
  assert(!unresolved_installed_target(active, active, true));
  const auto portable = root / "Portable/Extensions/SunshinePlaynite";
  manifest(portable);
  assert(!alternate_root(portable, active, old));
  assert(!unresolved_installed_target(portable, active, false));
  assert(!needs_migration(false, false));
  assert(needs_migration(false, true));
  const auto redirected = root / "redirected-active";
  std::error_code redirect_ec;
  fs::create_directory_symlink(active, redirected, redirect_ec);
  if (!redirect_ec) {
    assert(inspect_path(redirected) == path_inspection::unsafe);
    // With no alternate and no pending obligation the installer does not
    // impose the retirement marker's path guard on ordinary deployment.
    if (needs_migration(false, migration_pending(redirected))) assert(begin_migration(redirected, error));
    std::ofstream(redirected / "deployment-test") << "ok";
    assert(fs::exists(active / "deployment-test"));
  }
  assert(!alternate_root(root / "arbitrary-override", active, old));
  std::ofstream(old / "settings.json") << "user settings";
  assert(disable_duplicate(active, old, error));
  assert(!fs::exists(old / "extension.yaml"));
  assert(fs::exists(old / "extension.yaml.vibepollo-disabled"));
  assert(fs::exists(old / "settings.json") && fs::exists(old / "SunshinePlaynite.psm1"));
  assert(fs::exists(active / "extension.yaml"));
  assert(disable_duplicate(active, old, error)); // already migrated
  manifest(old, true); // repair/reinstall must not overwrite the previous backup
  assert(disable_duplicate(active, old, error));
  assert(fs::exists(old / "extension.yaml.vibepollo-disabled.1"));
  manifest(old); // stale compiled connectors are recognized too
  assert(disable_duplicate(active, old, error));
  manifest(old, true);
  fs::rename(active / "extension.yaml", active / "partial");
  assert(!disable_duplicate(active, old, error)); // replacement not activated
  assert(fs::exists(old / "extension.yaml"));
  fs::rename(active / "partial", active / "extension.yaml");
  assert(disable_duplicate(active, old, error)); // retry succeeds
  assert(disable_duplicate(active, active, error));
  assert(fs::exists(active / "extension.yaml"));
  manifest(old, true);
  for (const auto &text : {
       "Id: Another.Extension\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: ../other.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nId: Other\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "nested:\n  Id: Sunshine.Playnite.PowerShell\nType: Script\nModule: SunshinePlaynite.psm1\n",
       "Id: Sunshine.Playnite.PowerShell\nType: GenericPlugin\nModule: SunshinePlaynite.psm1\n"}) {
    std::ofstream(old / "extension.yaml") << text;
    assert(disable_duplicate(active, old, error));
    assert(fs::exists(old / "extension.yaml"));
  }
  manifest(old, true);
  assert(disable_duplicate(active, old.parent_path() / ".." / "Extensions" / "SunshinePlaynite", error));
  assert(fs::exists(old / "extension.yaml"));
  std::error_code ec;
  fs::create_directory_symlink(old, root / "linked", ec);
  if (!ec) {
    assert(disable_duplicate(active, root / "linked", error));
    assert(fs::exists(old / "extension.yaml"));
  }
  // A file sharing an expected directory name must never be treated as a plugin.
  std::ofstream(root / "not-directory") << "untouched";
  assert(disable_duplicate(active, root / "not-directory", error));
  // Force a recoverable failure without relying on Unix permissions/root status.
  for (unsigned i = 0; i < 1000; ++i)
    std::ofstream(old / ("extension.yaml.vibepollo-disabled" + (i ? "." + std::to_string(i) : ""))) << "previous backup";
  assert(!disable_duplicate(active, old, error));
  assert(fs::exists(old / "extension.yaml") && fs::exists(active / "extension.yaml"));
  fs::remove(old / "extension.yaml.vibepollo-disabled.999");
  assert(disable_duplicate(active, old, error));
  std::cout << "PASS: two-root migration, partial deployment, retry, preservation, ownership and path safety\n";
}
''')
    executable = root / "migration"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(repo), str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(root)], check=True, timeout=10)

    # Compile the installer's actual discovery/marker guard against simulated
    # registry and association outcomes; deployment is represented by a file.
    source = (repo / "src/platform/windows/playnite_integration.cpp").read_text()
    start = source.index("      const auto alternate = alternate_connector_dir(destDir);")
    end = source.index("      std::wstring playniteExe;", start)
    guard = source[start:end]
    cpp.write_text(r'''
#include "src/platform/windows/playnite_plugin_migration.h"
#include <cassert>
#include <fstream>
namespace fs = std::filesystem;
namespace migration = platf::playnite::migration;
std::optional<fs::path> alternate_fixture;
fs::path installed_fixture;
std::optional<fs::path> alternate_connector_dir(const fs::path &) { return alternate_fixture; }
bool resolve_extensions_dir_via_per_user_install(fs::path &out) { out = installed_fixture; return !out.empty(); }
bool guard(const fs::path &destDir, std::string &error_out) {
  std::error_code ec;
''' + guard + r'''
  return true;
}
int main(int, char **argv) {
  fs::path root = fs::path(argv[1]) / "guard", profile = root / "profile", portable = root / "portable";
  fs::create_directories(profile); fs::create_directories(portable);
  installed_fixture = profile;
  std::string error;
  std::ofstream(profile / "extension.yaml") << "Version: 0.5.2";
  alternate_fixture.reset(); // missing association, registry still resolves profile
  assert(!guard(profile, error));
  std::ifstream old(profile / "extension.yaml"); std::string contents; std::getline(old, contents);
  assert(contents == "Version: 0.5.2");
  assert(guard(portable, error)); // unrelated override is not treated as that installation
  alternate_fixture = root / "absent-alternate";
  fs::path redirected = root / "redirected";
  std::error_code ec; fs::create_directory_symlink(profile, redirected, ec);
  if (!ec) {
    assert(guard(redirected, error)); // no alternate: no marker path guard
    std::ofstream(redirected / "new-version") << "deployed";
    assert(fs::exists(profile / "new-version"));
  }
}
''')
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-I", str(repo), str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(root)], check=True, timeout=10)
    print("PASS: unresolved installed target and absent-alternate deployment guards")
