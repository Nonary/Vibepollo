"""Exercise actual SDK discovery and legacy upgrade policy without Windows."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

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
        script.write_text(prefix + discovery + f'\nif(NOT SUNSHINE_DOTNET_EXECUTABLE STREQUAL "{dotnet.as_posix()}")\nmessage(FATAL_ERROR "SDK discovery failed")\nendif()\n')
        subprocess.run([cmake, "-P", str(script)], check=True,
                       env=dict(os.environ, PATH=str(sdk), DOTNET_ROOT="", ProgramFiles=""))
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
