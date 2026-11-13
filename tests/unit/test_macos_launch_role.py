"""Compile actual macOS launch/resume gates against the current role enum."""
from pathlib import Path
import argparse,re,subprocess,tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',default='clang++');args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
source=(root/'src/nvhttp.cpp').read_text()
blocks=re.findall(r'#ifdef __APPLE__\n(      // Bring the client[^#]+?)#endif',source)
assert len(blocks)==2, 'Expected launch and resume display preparation gates'
role=re.search(r'enum class role_e[^;]+;', (root/'src/remote_session.h').read_text()).group()
base=r"""
#include <cassert>
#include <cstdint>
#include <memory>
namespace remote_session { @ROLE@ }
struct launch_t {remote_session::role_e role;};
int holds=0;
int requested_display_config(const launch_t &) {return 42;}
namespace platf::macos_virtual_display {void hold_for_launch(int value) {assert(value==42);++holds;}}
void prepare(const std::unique_ptr<launch_t> &launch_session) {
@GATE@
}
int main() {
 auto session=std::make_unique<launch_t>();
 for(const auto role : {remote_session::role_e::game,remote_session::role_e::monitor}) {
  holds=0;session->role=role;prepare(session);assert(holds==1);
 }
 holds=0;session->role=remote_session::role_e::input;prepare(session);assert(holds==0);
}
""".replace('@ROLE@',role)
with tempfile.TemporaryDirectory(prefix='macos-launch-role-') as directory:
 for index,gate in enumerate(blocks):
  source=Path(directory)/f'role-{index}.cpp';source.write_text(base.replace('@GATE@',gate));binary=source.with_suffix('')
  subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(source),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True)
  print(f'PASS: actual macOS gate {index}: game/monitor acquire display, Remote Input skips it')
