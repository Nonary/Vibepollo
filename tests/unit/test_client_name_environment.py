"""Compile the production client-name assignment and exact broker allowlist."""
from pathlib import Path
import argparse, re, subprocess, tempfile
parser=argparse.ArgumentParser();parser.add_argument('--compiler',default='clang++');args=parser.parse_args()
root=Path(__file__).resolve().parents[2]
header=next((root/'packaging/linux').glob('*-session-stream-environment.h'))
prefix=header.name.split('-session-stream-environment.h')[0]
source=(root/'src/process.cpp').read_text()
line=re.search(r'    _env\["SUNSHINE_CLIENT_NAME"\] = [^;]+;',source).group()
sender=(root/'src/platform/linux/misc.cpp').read_text()
start=sender.index('        size_t stream_environment_bytes = 0;')
forwarding=sender[start:sender.index('\n      }\n    } else {',start)]
program=r"""
#include <algorithm>
#include <cassert>
#include <string_view>
#include <system_error>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include "@HEADER@"
struct session_t {std::string client_name;};
struct log_t {template<class T>log_t &operator<<(const T &){return *this;}} logger;
#define BOOST_LOG(level) logger
struct entry_t {
 std::string name,value;
 const auto &get_name() const {return name;}
 auto to_string() const {return value;}
};
namespace bp {struct child {operator std::vector<std::string>() const {return {};}};}
std::vector<std::string> forward(const std::vector<entry_t> &env,std::error_code &ec) {
 std::vector<std::string> args;
@FORWARDING@
 return args;
}
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
 const std::vector<std::string> names {"Gaming\tPC", "Gaming\nPC", std::string("Gaming\0PC",9), std::string(1025,'x')};
 for (const auto &bad : names) {
  std::error_code ec;
  const auto result=forward({{"SUNSHINE_CLIENT_NAME",bad},{"SUNSHINE_CLIENT_WIDTH","1920"}},ec);
  assert(!ec && result==std::vector<std::string>{"SUNSHINE_CLIENT_WIDTH=1920"});
 }
 std::error_code ec;
 auto result=forward({{"SUNSHINE_CLIENT_NAME",launch_session->client_name},{"SUNSHINE_CLIENT_WIDTH","1920"}},ec);
 assert(!ec && result.size()==2 && result[0]=="SUNSHINE_CLIENT_NAME="+launch_session->client_name);
 result=forward({{"SUNSHINE_CLIENT_NAME","fine"},{"SUNSHINE_CLIENT_WIDTH","invalid"}},ec);
 assert(ec==std::errc::invalid_argument && result.empty());
}
""".replace('@HEADER@',str(header.relative_to(root))).replace('@PREFIX@',prefix).replace('@ASSIGNMENT@',line).replace('@FORWARDING@',forwarding)
with tempfile.TemporaryDirectory(prefix='client-name-env-') as tmp:
 cpp=Path(tmp)/'name.cpp';cpp.write_text(program);binary=Path(tmp)/'name'
 subprocess.run([args.compiler,'-std=c++23','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(root),str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
print('PASS: production client-name assignment, literal broker value, empty/1024/1025 bounds and control-character rejection; optional unrepresentable alias never blocks existing app launches')
