"""Compile launch color selection and the actual special-role call site."""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = (Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'src/nvhttp.cpp').read_text()
start = source.index('      auto color_app_ctx = launch_app_ctx;')
end = source.index(';', source.index('launch_session->prefer_sdr_10bit =', start)) + 1
selection = source[start:end]
start = source.index('make_launch_session_from_snapshot(false, false, args, verified_client, &request_client_identity')
call = source[start:source.index(';', start)]
with tempfile.TemporaryDirectory(prefix='resume-color-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(r'''
#include <cassert>
#include <memory>
#include <optional>
#include <string>
struct app { std::optional<bool> prefer_10bit_sdr; };
struct client { bool prefer_10bit_sdr; };
struct session { int appid=0; bool prefer_sdr_10bit=false; };
namespace remote_session {
enum class control_e { none, resume, running_game };
control_e selected = control_e::resume;
control_e identify(int, const std::string&, int) { return selected; }
}
namespace proc {
struct process {
 std::shared_ptr<app> current=std::make_shared<app>();
 int current_app_id() { return 42; }
 std::shared_ptr<app> resolve_app(int) { return current; }
} proc;
}
namespace rtsp_stream::hdr_request_policy {
bool resolve_prefer_10bit_sdr(bool client, std::optional<bool> app) { return app.value_or(client); }
}
std::shared_ptr<app> launch_app_ctx;
std::string launch_appuuid_arg;
std::shared_ptr<session> make_launch_session_from_snapshot(bool,bool,int,client* verified_client,int*,bool use_app_color_preference=true) {
 auto launch_session=std::make_shared<session>();
''' + selection + r'''
 return launch_session;
}
int main() {
 int args=0, request_client_identity=0;
 client value{}; auto* verified_client=&value;
 for (bool preference : {false,true}) {
  value.prefer_10bit_sdr=preference;
  for (std::optional<bool> app_preference : {std::optional<bool>{},std::optional<bool>{false},std::optional<bool>{true}}) {
   proc::proc.current->prefer_10bit_sdr=app_preference;
   auto special = ''' + call + r''';
   assert(special->prefer_sdr_10bit==preference);
   auto game=make_launch_session_from_snapshot(false,false,args,verified_client,&request_client_identity);
   assert(game->prefer_sdr_10bit==app_preference.value_or(preference));
  }
 }
}
''')
    subprocess.run(['g++','-std=c++17',str(path/'test.cpp'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
print('PASS: Resume uses game color preferences while retained Monitor preserves device preferences')
