"""Compile production IGD classification for old and current MiniUPnPc APIs."""
from pathlib import Path
import argparse
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='clang++')
args = parser.parse_args()
header = (root / 'src/upnp.h').read_text()
start = header.index('  enum class igd_status_e')
enum = header[start:header.index('  };', start) + 4]
source = (root / 'src/upnp.cpp').read_text()
start = source.index('  bool is_valid_igd(')
function = source[start:source.index('\n  class deinit_t', start)]
program = enum + function + r"""
#include <cassert>
int main() {
  for (int raw = -1; raw <= 100; ++raw) {
#if MINIUPNPC_API_VERSION >= 18
    const bool expected = raw >= 1 && raw <= 3;
    static_assert(static_cast<int>(igd_status_e::disconnected) == 3);
    static_assert(static_cast<int>(igd_status_e::unknown_device) == 4);
#else
    const bool expected = raw == 1 || raw == 2;
    static_assert(static_cast<int>(igd_status_e::disconnected) == 2);
    static_assert(static_cast<int>(igd_status_e::unknown_device) == 3);
#endif
    assert(is_valid_igd(static_cast<igd_status_e>(raw)) == expected);
  }
}
"""
with tempfile.TemporaryDirectory(prefix='upnp-igd-') as directory:
    cpp = Path(directory) / 'igd.cpp'
    cpp.write_text(program)
    for api in (17, 18, 19):
        binary = Path(directory) / f'igd-{api}'
        subprocess.run([args.compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        f'-DMINIUPNPC_API_VERSION={api}', str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
        print(f'PASS: production IGD classification with MiniUPnPc API {api}')
