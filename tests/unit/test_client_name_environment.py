"""Compile the production client-name assignment and exact broker allowlist."""
from pathlib import Path
import argparse, re, subprocess, tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',default='clang++');args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
header=next((root/'packaging/linux').glob('*-session-stream-environment.h'))
prefix=header.name.split('-session-stream-environment.h')[0]
source=(root/'src/process.cpp').read_text()
line=re.search(r'    _env\["SUNSHINE_CLIENT_NAME"\] = [^;]+;',source).group()
program=r"""
#include <cassert>
#include <map>
#include <memory>
#include <string>
#include "@HEADER@"
struct session_t {std::string client_name;};
int main() {
 std::map<std::string,std::string> _env;
 auto launch_session=std::make_unique<session_t>();
 launch_session->client_name="Paired Client = $HOME; $(false)";
@ASSIGNMENT@
 assert(_env["SUNSHINE_CLIENT_NAME"]==launch_session->client_name);
 std::string entry="SUNSHINE_CLIENT_NAME="+_env["SUNSHINE_CLIENT_NAME"];
 assert(@PREFIX@_stream_environment_entry_is_safe(entry.c_str(),nullptr));
 assert(@PREFIX@_stream_environment_entry_is_safe("SUNSHINE_CLIENT_NAME=",nullptr));
 assert(!@PREFIX@_stream_environment_entry_is_safe("SUNSHINE_CLIENT_NAME=bad\nname",nullptr));
 assert(!@PREFIX@_stream_environment_entry_is_safe("SUNSHINE_CLIENT_NAME=bad\rname",nullptr));
 assert(!@PREFIX@_stream_environment_entry_is_safe("SUNSHINE_CLIENT_NAME=bad\x01name",nullptr));
 entry="SUNSHINE_CLIENT_NAME="+std::string(1024,'x');
 assert(@PREFIX@_stream_environment_entry_is_safe(entry.c_str(),nullptr));
 entry+='x';
 assert(!@PREFIX@_stream_environment_entry_is_safe(entry.c_str(),nullptr));
 assert(!@PREFIX@_stream_environment_entry_is_safe("LD_PRELOAD=unsafe",nullptr));
}
""".replace('@HEADER@',str(header.relative_to(root))).replace('@PREFIX@',prefix).replace('@ASSIGNMENT@',line)
with tempfile.TemporaryDirectory(prefix='client-name-env-') as tmp:
 cpp=Path(tmp)/'name.cpp';cpp.write_text(program);binary=Path(tmp)/'name'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root),str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PASS: production client-name assignment, literal broker value, empty/1024/1025 bounds and control-character rejection')
