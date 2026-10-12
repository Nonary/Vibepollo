"""Exercise actual SDK discovery and legacy upgrade policy without Windows."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

repo = Path(__file__).resolve().parents[4]
source = (repo / "cmake/targets/windows.cmake").read_text()
begin = source.index("if(DEFINED SUNSHINE_DOTNET_EXECUTABLE")
end = source.index("endif()", source.index("if(NOT SUNSHINE_DOTNET_EXECUTABLE)", begin)) + len("endif()")
discovery = source[begin:end]
cmake = shutil.which("cmake")
compiler = shutil.which(os.environ.get("CXX", "g++"))
if not compiler:
    print("SKIP: C++ compiler unavailable")
    raise SystemExit(77)
with tempfile.TemporaryDirectory(prefix="playnite-upgrade-") as temporary:
    directory = Path(temporary)
    sdk = directory / "sdk"
    sdk.mkdir()
    dotnet = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
    dotnet.write_text("sdk discovery fixture")
    dotnet.chmod(0o755)
    for case, prefix in [
        ("path", ""),
        ("empty-cache", 'set(SUNSHINE_DOTNET_EXECUTABLE "" CACHE FILEPATH "SDK")\n'),
        ("explicit", f'set(SUNSHINE_DOTNET_EXECUTABLE "{dotnet.as_posix()}" CACHE FILEPATH "SDK")\n'),
    ]:
        script = directory / (case + ".cmake")
        # Clear discovery hints in CMake too: Windows child-process environments
        # may retain the well-known ProgramFiles value despite an empty override.
        fixture_env = 'set(ENV{DOTNET_ROOT} "")\nset(ENV{ProgramFiles} "")\n'
        script.write_text(fixture_env + prefix + discovery + f'\nif(NOT SUNSHINE_DOTNET_EXECUTABLE STREQUAL "{dotnet.as_posix()}")\nmessage(FATAL_ERROR "SDK discovery failed: expected [{dotnet.as_posix()}], found [${{SUNSHINE_DOTNET_EXECUTABLE}}]")\nendif()\n')
        # Windows environment names are case-insensitive, but a plain dict can
        # contain both PROGRAMFILES and ProgramFiles. Remove inherited spellings
        # before overriding hints so an installed SDK cannot mask the PATH case.
        discovery_env = {key: value for key, value in os.environ.items()
                         if key.casefold() not in {"path", "dotnet_root", "programfiles"}}
        discovery_env.update(PATH=str(sdk), DOTNET_ROOT="", ProgramFiles="")
        subprocess.run([cmake, "-P", str(script)], check=True, env=discovery_env)
        print("PASS: SDK discovery", case)

    # Compile the production response calculation with a minimal JSON boolean
    # fixture; the normal semver helper above it is included unchanged.
    status = (repo / "src/confighttp_playnite.cpp").read_text()
    helpers = status[status.index("    auto normalize_ver ="):status.index("    std::string installed_ver, packaged_ver;")]
    policy = status[status.index("    bool update_available ="):status.index('    out["update_available"] =')]
    cpp = directory / "policy.cpp"
    cpp.write_text('''#include <algorithm>
#include <string>
#include <vector>
#include <stdexcept>
struct Value { bool value; bool is_boolean() { return true; } template<class T> T get() { return value; } };
struct Output { Value installed; Value &operator[](const char *) { return installed; } };
bool update(bool legacy, bool installed, bool known, std::string installed_ver, std::string packaged_ver) {
  struct { bool legacy_plugin; } install_state{legacy};
  Output out{{installed}};
  bool have_installed = known, have_packaged = known;
''' + helpers + policy + '''return update_available;
}
int main() {
  if (!update(true, false, true, "0.4.14", "0.5.0")) return 1;
  if (!update(true, false, true, "0.5.0", "0.5.0")) return 2;
  if (!update(true, false, false, "", "")) return 3;
  if (!update(false, true, true, "0.4.14", "0.5.0")) return 4;
  if (update(false, true, true, "0.5.0", "0.5.0")) return 5;
  if (update(false, false, true, "", "0.5.0")) return 6;
  if (update(false, true, false, "", "")) return 7;
}
''')
    executable = directory / ("policy.exe" if os.name == "nt" else "policy")
    subprocess.run([compiler, "-std=c++20", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=5)
    print("PASS: legacy migration and compiled plugin version policy")

    # Exercise actual filesystem classification and post-install cleanup. The
    # port changes which DLL is current; cleanup must preserve that DLL.
    assembly = ET.parse(repo / "plugins/playnite/SunshinePlaynite/SunshinePlaynite.csproj").findtext(".//AssemblyName")
    current_dll = assembly + ".dll"
    query = status[status.index("  struct playnite_install_state_t"):status.index("  // Enhance app JSON")]
    installer = (repo / "src/platform/windows/playnite_integration.cpp").read_text()
    cleanup_start = installer.index("      if (deployed)", installer.index("      cleanup_guard.disable();"))
    cleanup = installer[cleanup_start:installer.index("      if (!deployed)", cleanup_start)]
    cpp.write_text('''#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
std::string target;
bool resolved = true;
namespace platf::playnite {
bool get_extension_target_dir(std::string &out) { out = target; return resolved; }
}
''' + query + '''
bool remove_legacy(const std::filesystem::path &destDir) {
  std::error_code ec;
  bool deployed = true;
''' + cleanup + '''return deployed;
}
int main(int argc, char **argv) {
  const std::filesystem::path dir{argv[1]};
  target = dir.string();
  const std::string current = "''' + current_dll + '''";
  for (const std::string file : {current, std::string{"SunshinePlaynite.psm1"}, std::string{"SunshinePlaynite.dll"}, std::string{"VibeshinePlaynite.dll"}}) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directory(dir);
    std::ofstream(dir / "extension.yaml") << "manifest";
    std::ofstream(dir / file) << "module";
    auto state = query_plugin_install_state(false);
    bool is_current = file == current;
    if (state.installed != is_current || state.legacy_plugin == is_current) return 1;
  }
  std::ofstream(dir / current) << "current module";
  std::ofstream(dir / "SunshinePlaynite.psm1") << "legacy script";
  if (!remove_legacy(dir) || !std::filesystem::exists(dir / current) || std::filesystem::exists(dir / "SunshinePlaynite.psm1")) return 2;
  resolved = false;
  if (query_plugin_install_state(false).installed.has_value()) return 3;
  if (query_plugin_install_state(true).installed != true) return 4;
}
''')
    subprocess.run([compiler, "-std=c++20", str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable), str(directory / "extension")], check=True, timeout=5)
    print("PASS: installed module classification and cleanup preserve current DLL")
