"""Execute the production Linux metadata block without capture or desktop services."""
import pathlib
import subprocess
import sys
import tempfile


root = pathlib.Path(__file__).resolve().parents[4]
source = (root / "src/confighttp.cpp").read_text()
metadata = source.split("  void getMetadata(", 1)[1]
block = metadata.split('    const char *session_role =', 1)[1]
block = '    const char *session_role =' + block.split('    const bool managed_active =', 1)[0]
brand = "VIBE" + "SHINE" if 'VIBESHINE_SESSION_ROLE' in block else "VIBEPOLLO"

fixture = r'''
#include <cassert>
#include <cstdlib>
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>
nlohmann::json metadata() {
  nlohmann::json output_tree;
''' + block + r'''
  return output_tree;
}
int main() {
  const char *machine = "BRAND_MACHINE_HOST";
  const char *role = "BRAND_SESSION_ROLE";
  unsetenv(machine);
  unsetenv(role);
  assert(metadata()["linux"].at("machine_host") == false);
  assert(metadata()["linux"]["session_role"] == "unknown");
  for (const char *value : {"", "0", "true", "10", "1extra"}) {
    setenv(machine, value, 1);
    assert(metadata()["linux"].at("machine_host") == false);
  }
  setenv(machine, "1", 1);
  for (const char *value : {"desktop", "greeter", "unknown"}) {
    setenv(role, value, 1);
    assert(metadata()["linux"].at("machine_host") == true);
    assert(metadata()["linux"]["session_role"] == value);
  }
  unsetenv(machine);
  setenv(role, "desktop", 1);
  assert(metadata()["linux"].at("machine_host") == false);
}
'''
fixture = fixture.replace("BRAND_", brand + "_")
with tempfile.TemporaryDirectory(prefix="machine-tray-metadata-") as temporary:
    test_source = pathlib.Path(temporary) / "metadata.cpp"
    executable = pathlib.Path(temporary) / "metadata"
    test_source.write_text(fixture)
    includes = ["-I" + path for path in sys.argv[2:]]
    subprocess.run([sys.argv[1] if len(sys.argv) > 1 else "c++", "-std=c++17", "-O0", *includes,
                    str(test_source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
